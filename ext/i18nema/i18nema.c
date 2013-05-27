#include <ruby.h>
#include "syck.h"
#include "uthash.h"

VALUE I18nema = Qnil;

struct i_object;
struct i_key_value;
static VALUE array_to_rarray(struct i_object *array);
static VALUE hash_to_rhash(struct i_object *hash);
static void merge_hash(struct i_object *hash, struct i_object *other_hash);
static void delete_hash(struct i_key_value *hash);


enum i_object_type {
  i_type_string,
  i_type_array,
  i_type_hash
};

union i_object_data {
  char *string;
  struct i_object **array;
  struct i_key_value *hash;
};

typedef struct i_object
{
  unsigned long size;
  enum i_object_type type;
  union i_object_data data;
} i_object_t;

typedef struct i_key_value
{
  char *key;
  struct i_object *value;
  UT_hash_handle hh;
} i_key_value_t;

/*
 * currently there is just a single translations object
 * TODO: support multiple (i.e. one per backend instance)
 */
i_object_t *translations = NULL;
int current_translation_count = 0;
int total_translation_count = 0;

static VALUE
i_object_to_robject(i_object_t *object) {
  if (object == NULL)
    return Qnil;
  switch (object->type) {
  case i_type_string:
    return rb_str_new2(object->data.string);
  case i_type_array:
    return array_to_rarray(object);
  case i_type_hash:
    return hash_to_rhash(object);
  }
  return Qnil;
}

static VALUE
array_to_rarray(i_object_t *array)
{
  VALUE result = rb_ary_new2(array->size);
  for (unsigned long i = 0; i < array->size; i++)
    rb_ary_store(result, i, i_object_to_robject(array->data.array[i]));
  return result;
}

static VALUE
hash_to_rhash(i_object_t *hash)
{
  i_key_value_t *handle = hash->data.hash;
  VALUE result = rb_hash_new();
  for (; handle != NULL; handle = handle->hh.next)
    rb_hash_aset(result, rb_str_new2(handle->key), i_object_to_robject(handle->value));
  return result;
}

/*
 *  call-seq:
 *     I18nema.direct_lookup([part]+)  -> localized_str
 *
 *  Returns the translation(s) found under the specified key.
 *
 *     I18nema.direct_lookup("en", "foo", "bar")   #=> "lol"
 *     I18nema.direct_lookup("en", "foo")          #=> {"bar"=>"lol", "baz"=>["asdf", "qwerty"]}
 */

static VALUE
direct_lookup(int argc, VALUE *argv)
{
  i_object_t *result = translations;
  i_key_value_t *kv = NULL;
  char *s;

  for (int i = 0; i < argc && result != NULL && result->type == i_type_hash; i++) {
    s = StringValueCStr(argv[i]);
    HASH_FIND_STR(result->data.hash, s, kv);
    result = kv == NULL ? NULL : kv->value;
  }

  return i_object_to_robject(result);
}

static void
delete_object(i_object_t *object)
{
  if (object == NULL)
    return;

  switch (object->type) {
  case i_type_string:
    free(object->data.string);
    break;
  case i_type_array:
    for (unsigned long i = 0; i < object->size; i++)
      delete_object(object->data.array[i]);
    free(object->data.array);
    break;
  case i_type_hash:
    delete_hash(object->data.hash);
    break;
  }
  free(object);
}

static void
delete_key_value(i_key_value_t *kv, int delete_value)
{
  if (delete_value)
    delete_object(kv->value);
  free(kv->key);
  free(kv);
}

static void
delete_hash(i_key_value_t *hash)
{
  i_key_value_t *kv, *tmp;
  HASH_ITER(hh, hash, kv, tmp) {
    delete_key_value(kv, 1);
  }
}

static void
add_key_value(i_key_value_t **hash, i_key_value_t *kv)
{
  i_key_value_t *existing = NULL;
  HASH_FIND_STR(*hash, kv->key, existing);

  if (existing != NULL) {
    if (existing->value->type == i_type_hash && kv->value->type == i_type_hash) {
      merge_hash(existing->value, kv->value);
      delete_key_value(kv, 0);
      return;
    }
    HASH_DEL(*hash, existing);
    delete_key_value(existing, 1);
  }
  HASH_ADD_KEYPTR(hh, *hash, kv->key, strlen(kv->key), kv);
}

static void
merge_hash(i_object_t *hash, i_object_t *other_hash)
{
  i_key_value_t *kv, *tmp;

  HASH_ITER(hh, other_hash->data.hash, kv, tmp) {
    HASH_DEL(other_hash->data.hash, kv);
    add_key_value(&hash->data.hash, kv);
  }
  delete_object(other_hash);
}

static SYMID
handle_syck_node(SyckParser *parser, SyckNode *node)
{
  i_object_t *result;
  result = malloc(sizeof(i_object_t));
  SYMID oid;

  switch (node->kind) {
  case syck_str_kind:
    // TODO: why are there sometimes empty string nodes (memory leak, since they never get used)
    result->type = i_type_string;
    result->size = node->data.str->len;
    result->data.string = malloc(node->data.str->len + 1);
    strncpy(result->data.string, node->data.str->ptr, node->data.str->len);
    result->data.string[node->data.str->len] = '\0';
    break;
  case syck_seq_kind:
    result->type = i_type_array;
    result->size = node->data.list->idx;
    result->data.array = malloc(node->data.list->idx * sizeof(i_object_t*));
    for (long i = 0; i < node->data.list->idx; i++) {
      i_object_t *item = NULL;

      oid = syck_seq_read(node, i);
      syck_lookup_sym(parser, oid, (char **)&item);
      if (item->type == i_type_string)
        current_translation_count++;
      result->data.array[i] = item;
    }
    break;
  case syck_map_kind:
    result->type = i_type_hash;
    result->size = node->data.pairs->idx;
    result->data.hash = NULL;
    for (long i = 0; i < node->data.pairs->idx; i++) {
      i_object_t *key = NULL, *value = NULL;

      oid = syck_map_read(node, map_key, i);
      syck_lookup_sym(parser, oid, (char **)&key);
      oid = syck_map_read(node, map_value, i);
      syck_lookup_sym(parser, oid, (char **)&value);

      i_key_value_t *kv;
      kv = malloc(sizeof(*kv));
      kv->key = key->data.string;
      free(key);
      kv->value = value;
      if (value->type == i_type_string)
        current_translation_count++;
      add_key_value(&result->data.hash, kv);
    }
    break;
  }


  return syck_add_sym(parser, (char *)result);
}

/*
 *  call-seq:
 *     I18nema.load_yaml_string(yaml_str) -> num_translations
 *
 *  Loads translations from the specified yaml string, and returns the
 *  number of (new) translations stored.
 *
 *     I18nema.load_yaml_string("en:\n  foo: bar")   #=> 1
 */

static VALUE
load_yml_string(VALUE self, VALUE yml)
{
  SYMID oid;
  i_object_t *current_translations = NULL;
  current_translation_count = 0;
  SyckParser* parser = syck_new_parser();
  syck_parser_handler(parser, handle_syck_node);
  StringValue(yml);
  syck_parser_str(parser, RSTRING_PTR(yml), RSTRING_LEN(yml), NULL);
  // TODO: implement these
  //syck_parser_bad_anchor_handler(parser, syck_badanchor_handler);
  //syck_parser_error_handler(parser, syck_error_handler);

  oid = syck_parse(parser);
  syck_lookup_sym(parser, oid, (char **)&current_translations);
  syck_free_parser(parser);
  if (translations == NULL)
    translations = current_translations;
  else
    merge_hash(translations, current_translations);
  total_translation_count += current_translation_count;

  return INT2NUM(current_translation_count);
}

/*
 *  call-seq:
 *     I18nema.available_locales -> locales
 *
 *  Returns the currently loaded locales. Order is not guaranteed.
 *
 *     I18nema.available_locales   #=> ["en", "es"]
 */

static VALUE
available_locales()
{
  i_key_value_t *current = translations->data.hash;
  VALUE ary = rb_ary_new2(0);

  for (; current != NULL; current = current->hh.next)
    rb_ary_push(ary, rb_str_new2(current->key));

  return ary;
}

/*
 *  call-seq:
 *     I18nema.reset! -> true
 *
 *  Clears out all currently stored translations.
 *
 *     I18nema.reset!   #=> true
 */

static VALUE
reset()
{
  delete_object(translations);
  translations = NULL;
  total_translation_count = 0;
  return Qtrue;
}

void
Init_i18nema()
{
  I18nema = rb_define_module("I18nema");
  rb_define_singleton_method(I18nema, "direct_lookup", direct_lookup, -1);
  rb_define_singleton_method(I18nema, "load_yml_string", load_yml_string, 1);
  rb_define_singleton_method(I18nema, "available_locales", available_locales, 0);
  rb_define_singleton_method(I18nema, "reset!", reset, 0);
}

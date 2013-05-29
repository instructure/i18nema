#include <ruby.h>
#include "syck.h"
#include "uthash.h"

VALUE I18nema = Qnil,
      I18nemaBackend = Qnil,
      I18nemaBackendLoadError = Qnil;

struct i_object;
struct i_key_value;
static VALUE array_to_rarray(struct i_object *array);
static VALUE hash_to_rhash(struct i_object *hash);
static void merge_hash(struct i_object *hash, struct i_object *other_hash);
static void delete_hash(struct i_key_value **hash);
static void delete_object(struct i_object *object);

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

int current_translation_count = 0;

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

static i_object_t*
root_object_get(VALUE self)
{
  i_object_t *root_object;
  VALUE translations;
  translations = rb_iv_get(self, "@translations");
  Data_Get_Struct(translations, i_object_t, root_object);
  return root_object;
}

/*
 *  call-seq:
 *     backend.direct_lookup([part]+)  -> localized_str
 *
 *  Returns the translation(s) found under the specified key.
 *
 *     backend.direct_lookup("en", "foo", "bar")   #=> "lol"
 *     backend.direct_lookup("en", "foo")          #=> {"bar"=>"lol", "baz"=>["asdf", "qwerty"]}
 */

static VALUE
direct_lookup(int argc, VALUE *argv, VALUE self)
{
  i_object_t *result = root_object_get(self);;
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
empty_object(i_object_t *object)
{
  if (object == NULL)
    return;

  switch (object->type) {
  case i_type_string:
    xfree(object->data.string);
    break;
  case i_type_array:
    for (unsigned long i = 0; i < object->size; i++)
      delete_object(object->data.array[i]);
    xfree(object->data.array);
    break;
  case i_type_hash:
    delete_hash(&object->data.hash);
    break;
  }
}

static void
delete_object(i_object_t *object)
{
  empty_object(object);
  xfree(object);
}

static void
delete_key_value(i_key_value_t *kv, int delete_value)
{
  if (delete_value)
    delete_object(kv->value);
  xfree(kv->key);
  xfree(kv);
}

static void
delete_hash(i_key_value_t **hash)
{
  i_key_value_t *kv, *tmp;
  HASH_ITER(hh, *hash, kv, tmp) {
    HASH_DEL(*hash, kv);
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

static int
delete_syck_object(char *key, char *value, char *arg)
{
  i_object_t *object = (i_object_t *)value;
  delete_object(object);
  return ST_DELETE;
}

static void
handle_syck_error(SyckParser *parser, const char *str)
{
  char *endl = parser->cursor;
  while (*endl != '\0' && *endl != '\n')
    endl++;
  endl[0] = '\0';

  if (parser->syms)
    st_foreach(parser->syms, delete_syck_object, 0);
  rb_raise(I18nemaBackendLoadError, "%s on line %d, col %ld: `%s'", str, parser->linect + 1, parser->cursor - parser->lineptr, parser->lineptr);
}

static SyckNode*
handle_syck_badanchor(SyckParser *parser, const char *anchor)
{
  char error[strlen(anchor) + 14];
  sprintf(error, "bad anchor `%s'", anchor);
  handle_syck_error(parser, error);
  return NULL;
}

static SYMID
handle_syck_node(SyckParser *parser, SyckNode *node)
{
  i_object_t *result;
  result = ALLOC(i_object_t);
  SYMID oid;

  switch (node->kind) {
  case syck_str_kind:
    // TODO: why does syck sometimes give us empty string nodes? (small) memory leak, since they never end up in a seq/map
    result->type = i_type_string;
    result->size = node->data.str->len;
    result->data.string = xmalloc(node->data.str->len + 1);
    strncpy(result->data.string, node->data.str->ptr, node->data.str->len);
    result->data.string[node->data.str->len] = '\0';
    break;
  case syck_seq_kind:
    result->type = i_type_array;
    result->size = node->data.list->idx;
    result->data.array = ALLOC_N(i_object_t*, node->data.list->idx);
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
    result->data.hash = NULL;
    for (long i = 0; i < node->data.pairs->idx; i++) {
      i_object_t *key = NULL, *value = NULL;

      oid = syck_map_read(node, map_key, i);
      syck_lookup_sym(parser, oid, (char **)&key);
      oid = syck_map_read(node, map_value, i);
      syck_lookup_sym(parser, oid, (char **)&value);

      i_key_value_t *kv;
      kv = ALLOC(i_key_value_t);
      kv->key = key->data.string;
      xfree(key);
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
 *     backend.load_yaml_string(yaml_str) -> num_translations
 *
 *  Loads translations from the specified yaml string, and returns the
 *  number of (new) translations stored.
 *
 *     backend.load_yaml_string("en:\n  foo: bar")   #=> 1
 */

static VALUE
load_yml_string(VALUE self, VALUE yml)
{
  SYMID oid;
  i_object_t *root_object = root_object_get(self);
  i_object_t *new_root_object = NULL;
  current_translation_count = 0;
  SyckParser* parser = syck_new_parser();
  syck_parser_handler(parser, handle_syck_node);
  StringValue(yml);
  syck_parser_str(parser, RSTRING_PTR(yml), RSTRING_LEN(yml), NULL);
  syck_parser_bad_anchor_handler(parser, handle_syck_badanchor);
  syck_parser_error_handler(parser, handle_syck_error);

  oid = syck_parse(parser);
  syck_lookup_sym(parser, oid, (char **)&new_root_object);
  syck_free_parser(parser);
  if (new_root_object == NULL || new_root_object->type != i_type_hash) {
    delete_object(new_root_object);
    rb_raise(I18nemaBackendLoadError, "root yml node is not a hash");
  }
  merge_hash(root_object, new_root_object);

  return INT2NUM(current_translation_count);
}

/*
 *  call-seq:
 *     backend.available_locales -> locales
 *
 *  Returns the currently loaded locales. Order is not guaranteed.
 *
 *     backend.available_locales   #=> [:en, :es]
 */

static VALUE
available_locales(VALUE self)
{
  if (!RTEST(rb_iv_get(self, "@initialized")))
    rb_funcall(self, rb_intern("init_translations"), 0);
  i_object_t *root_object = root_object_get(self);
  i_key_value_t *current = root_object->data.hash;
  VALUE ary = rb_ary_new2(0);

  for (; current != NULL; current = current->hh.next)
    rb_ary_push(ary, rb_str_intern(rb_str_new2(current->key)));

  return ary;
}

/*
 *  call-seq:
 *     backend.reload! -> true
 *
 *  Clears out all currently stored translations.
 *
 *     backend.reload!   #=> true
 */

static VALUE
reload(VALUE self)
{
  i_object_t *root_object = root_object_get(self);
  empty_object(root_object);
  rb_iv_set(self, "@initialized", Qfalse);
  root_object = NULL;
  return Qtrue;
}

static VALUE
initialize(VALUE self)
{
  VALUE translations;
  i_object_t *root_object = ALLOC(i_object_t);
  root_object->type = i_type_hash;
  root_object->data.hash = NULL;
  translations = Data_Wrap_Struct(I18nemaBackend, 0, delete_object, root_object);
  rb_iv_set(self, "@translations", translations);
  return self;
}

void
Init_i18nema()
{
  I18nema = rb_define_module("I18nema");
  I18nemaBackend = rb_define_class_under(I18nema, "Backend", rb_cObject);
  I18nemaBackendLoadError = rb_define_class_under(I18nemaBackend, "LoadError", rb_eStandardError);
  rb_define_method(I18nemaBackend, "initialize", initialize, 0);
  rb_define_method(I18nemaBackend, "load_yml_string", load_yml_string, 1);
  rb_define_method(I18nemaBackend, "available_locales", available_locales, 0);
  rb_define_method(I18nemaBackend, "reload!", reload, 0);
  rb_define_method(I18nemaBackend, "direct_lookup", direct_lookup, -1);
}

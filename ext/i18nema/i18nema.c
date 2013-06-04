#include <ruby.h>
#include <ruby/encoding.h>
#include "vendor/syck.h"
#include "vendor/uthash.h"

#define CAN_FREE(item) item != NULL && item->type != i_type_true && item->type != i_type_false && item->type != i_type_null

VALUE I18nema = Qnil,
      I18nemaBackend = Qnil,
      I18nemaBackendLoadError = Qnil;

struct i_object;
struct i_key_value;
static VALUE array_to_rarray(struct i_object *array);
static VALUE hash_to_rhash(struct i_object *hash);
static void merge_hash(struct i_object *hash, struct i_object *other_hash);
static void delete_hash(struct i_key_value **hash, int recurse);
static void delete_object(struct i_object *object, int recurse);
static void delete_object_r(struct i_object *object);
static VALUE normalize_key(VALUE self, VALUE key, VALUE separator);

enum i_object_type {
  i_type_unused,
  i_type_string,
  i_type_array,
  i_type_hash,
  i_type_int,
  i_type_float,
  i_type_symbol,
  i_type_true,
  i_type_false,
  i_type_null
};

union i_object_data {
  char *string;
  struct i_object *array;
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

static int current_translation_count = 0;
static ID s_init_translations,
          s_to_f,
          s_to_s,
          s_to_sym;
static i_object_t i_object_null,
                  i_object_true,
                  i_object_false;

static VALUE
i_object_to_robject(i_object_t *object) {
  VALUE s;
  if (object == NULL)
    return Qnil;
  switch (object->type) {
  case i_type_string:
    return rb_enc_str_new(object->data.string, object->size, rb_utf8_encoding());
  case i_type_array:
    return array_to_rarray(object);
  case i_type_hash:
    return hash_to_rhash(object);
  case i_type_int:
    return rb_cstr2inum(object->data.string, 10);
  case i_type_float:
    s = rb_str_new(object->data.string, object->size);
    return rb_funcall(s, s_to_f, 0);
  case i_type_symbol:
    return ID2SYM(rb_intern(object->data.string));
  case i_type_true:
    return Qtrue;
  case i_type_false:
    return Qfalse;
  default:
    return Qnil;
  }
}

static VALUE
array_to_rarray(i_object_t *array)
{
  VALUE result = rb_ary_new2(array->size);
  for (unsigned long i = 0; i < array->size; i++)
    rb_ary_store(result, i, i_object_to_robject(&array->data.array[i]));
  return result;
}

static VALUE
hash_to_rhash(i_object_t *hash)
{
  i_key_value_t *handle = hash->data.hash;
  VALUE result = rb_hash_new();
  for (; handle != NULL; handle = handle->hh.next)
    rb_hash_aset(result, ID2SYM(rb_intern(handle->key)), i_object_to_robject(handle->value));
  return result;
}

static i_object_t*
i_object_get(VALUE self, const char *iv)
{
  i_object_t *object;
  VALUE wrapped;
  wrapped = rb_iv_get(self, iv);
  Data_Get_Struct(wrapped, i_object_t, object);
  return object;
}

static i_object_t*
translations_get(VALUE self)
{
  return i_object_get(self, "@translations");
}

static i_object_t*
normalized_key_cache_get(VALUE self)
{
  return i_object_get(self, "@normalized_key_cache");
}

static i_object_t*
hash_get(i_object_t *current, VALUE *keys, int num_keys)
{
  i_key_value_t *kv = NULL;
  for (int i = 0; i < num_keys && current != NULL && current->type == i_type_hash; i++) {
    Check_Type(keys[i], T_STRING);
    HASH_FIND_STR(current->data.hash, StringValueCStr(keys[i]), kv);
    current = kv == NULL ? NULL : kv->value;
  }
  return current;
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
  i_object_t *translations = translations_get(self);
  return i_object_to_robject(hash_get(translations, argv, argc));
}

static void
empty_object(i_object_t *object, int recurse)
{
  if (object == NULL)
    return;

  switch (object->type) {
  case i_type_array:
    if (recurse)
      for (unsigned long i = 0; i < object->size; i++)
        empty_object(&object->data.array[i], 1);
    xfree(object->data.array);
    break;
  case i_type_hash:
    delete_hash(&object->data.hash, recurse);
    break;
  case i_type_unused:
    break;
  default:
    xfree(object->data.string);
    break;
  }
}

static void
delete_object(i_object_t *object, int recurse)
{
  empty_object(object, recurse);
  if (CAN_FREE(object))
    xfree(object);
}

static void
delete_object_r(i_object_t *object)
{
  delete_object(object, 1);
}

static void
delete_key_value(i_key_value_t *kv, int delete_value)
{
  if (delete_value)
    delete_object_r(kv->value);
  xfree(kv->key);
  xfree(kv);
}

static void
delete_hash(i_key_value_t **hash, int recurse)
{
  i_key_value_t *kv, *tmp;
  HASH_ITER(hh, *hash, kv, tmp) {
    HASH_DEL(*hash, kv);
    delete_key_value(kv, recurse);
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
  delete_object_r(other_hash);
}

static int
delete_syck_st_entry(char *key, char *value, char *arg)
{
  i_object_t *object = (i_object_t *)value;
  // key object whose string we have yoinked into a kv, or item that
  // has been copied into an array
  if (object->type == i_type_unused)
    delete_object_r(object);
  return ST_DELETE;
}

static int
delete_syck_object(char *key, char *value, char *arg)
{
  i_object_t *object = (i_object_t *)value;
  delete_object(object, 0); // objects are in the syck symbol table, thus we don't want to double-free
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
handle_syck_badanchor(SyckParser *parser, char *anchor)
{
  char error[strlen(anchor) + 14];
  sprintf(error, "bad anchor `%s'", anchor);
  handle_syck_error(parser, error);
  return NULL;
}

static char*
new_string(char *orig, long len)
{
  char *str = xmalloc(len + 1);
  strncpy(str, orig, len);
  str[len] = '\0';
  return str;
}

static void
set_string_object(i_object_t *object, char *str, long len)
{
  object->type = i_type_string;
  object->size = len;
  object->data.string = new_string(str, len);
}

static i_object_t*
new_string_object(char *str, long len)
{
  i_object_t *object = ALLOC(i_object_t);
  set_string_object(object, str, len);
  return object;
}

static i_object_t*
new_array_object(long size)
{
  i_object_t *object = ALLOC(i_object_t);
  object->type = i_type_array;
  object->size = size;
  object->data.array = ALLOC_N(i_object_t, size);
  return object;
}

static i_object_t*
new_hash_object()
{
  i_object_t *object = ALLOC(i_object_t);
  object->type = i_type_hash;
  object->data.hash = NULL;
  return object;
}

static i_key_value_t*
new_key_value(char *key, i_object_t *value)
{
  i_key_value_t *kv = ALLOC(i_key_value_t);
  kv->key = key;
  kv->value = value;
  return kv;
}

static SYMID
handle_syck_node(SyckParser *parser, SyckNode *node)
{
  i_object_t *result;
  SYMID oid;

  switch (node->kind) {
  case syck_str_kind:
    if (node->type_id == NULL) {
      result = new_string_object(node->data.str->ptr, node->data.str->len);
    } else if (strcmp(node->type_id, "null") == 0) {
      result = &i_object_null;
    } else if (strcmp(node->type_id, "bool#yes") == 0) {
      result = &i_object_true;
    } else if (strcmp(node->type_id, "bool#no") == 0) {
      result = &i_object_false;
    } else if (strcmp(node->type_id, "int") == 0) {
      syck_str_blow_away_commas(node);
      result = new_string_object(node->data.str->ptr, node->data.str->len);
      result->type = i_type_int;
    } else if (strcmp(node->type_id, "float#fix") == 0 || strcmp(node->type_id, "float#exp") == 0) {
      syck_str_blow_away_commas(node);
      result = new_string_object(node->data.str->ptr, node->data.str->len);
      result->type = i_type_float;
    } else if (node->data.str->style == scalar_plain && node->data.str->len > 1 && strncmp(node->data.str->ptr, ":", 1) == 0) {
      result = new_string_object(node->data.str->ptr + 1, node->data.str->len - 1);
      result->type = i_type_symbol;
    } else {
      // legit strings, and everything else get the string treatment (binary, int#hex, timestamp, etc.)
      result = new_string_object(node->data.str->ptr, node->data.str->len);
    }
    break;
  case syck_seq_kind:
    result = new_array_object(node->data.list->idx);
    for (long i = 0; i < node->data.list->idx; i++) {
      i_object_t *item = NULL;

      oid = syck_seq_read(node, i);
      syck_lookup_sym(parser, oid, (void **)&item);
      if (item->type == i_type_string)
        current_translation_count++;
      memcpy(&result->data.array[i], item, sizeof(i_object_t));
      if (CAN_FREE(item))
        item->type = i_type_unused;
    }
    break;
  case syck_map_kind:
    result = new_hash_object();
    for (long i = 0; i < node->data.pairs->idx; i++) {
      i_object_t *key = NULL, *value = NULL;

      oid = syck_map_read(node, map_key, i);
      syck_lookup_sym(parser, oid, (void **)&key);
      oid = syck_map_read(node, map_value, i);
      syck_lookup_sym(parser, oid, (void **)&value);

      i_key_value_t *kv = new_key_value(key->data.string, value);
      key->type = i_type_unused; // so we know to free this node in delete_syck_st_entry
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
  i_object_t *root_object = translations_get(self);
  i_object_t *new_root_object = NULL;
  current_translation_count = 0;
  SyckParser* parser = syck_new_parser();
  syck_parser_handler(parser, handle_syck_node);
  StringValue(yml);
  syck_parser_str(parser, RSTRING_PTR(yml), RSTRING_LEN(yml), NULL);
  syck_parser_bad_anchor_handler(parser, handle_syck_badanchor);
  syck_parser_error_handler(parser, handle_syck_error);

  oid = syck_parse(parser);
  syck_lookup_sym(parser, oid, (void **)&new_root_object);
  if (parser->syms)
    st_foreach(parser->syms, delete_syck_st_entry, 0);
  syck_free_parser(parser);
  if (new_root_object == NULL || new_root_object->type != i_type_hash) {
    delete_object_r(new_root_object);
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
    rb_funcall(self, s_init_translations, 0);
  i_object_t *root_object = translations_get(self);
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
  i_object_t *root_object = translations_get(self);
  empty_object(root_object, 1);
  rb_iv_set(self, "@initialized", Qfalse);
  return Qtrue;
}

static VALUE
join_array_key(VALUE self, VALUE key, VALUE separator)
{
  long len = RARRAY_LEN(key);
  if (len == 0)
    return rb_str_new("", 0);

  VALUE ret = rb_ary_join(normalize_key(self, RARRAY_PTR(key)[0], separator), separator);
  for (long i = 1; i < len; i++) {
    rb_str_concat(ret, separator);
    rb_str_concat(ret, rb_ary_join(normalize_key(self, RARRAY_PTR(key)[i], separator), separator));
  }
  return ret;
}

/*
 *  call-seq:
 *     backend.normalize_key(key, separator) -> key
 *
 *  Normalizes and splits a key based on the separator.
 *
 *     backend.normalize_key "asdf", "."    #=> ["asdf"]
 *     backend.normalize_key "a.b.c", "."   #=> ["a", "b", "c"]
 *     backend.normalize_key "a.b.c", ":"   #=> ["a.b.c"]
 *     backend.normalize_key %{a b.c}, "."  #=> ["a", "b", "c"]
 */

static VALUE
normalize_key(VALUE self, VALUE key, VALUE separator)
{
  Check_Type(separator, T_STRING);

  i_object_t *key_map = normalized_key_cache_get(self),
             *sub_map = hash_get(key_map, &separator, 1);
  if (sub_map == NULL) {
    sub_map = new_hash_object();
    char *key = new_string(RSTRING_PTR(separator), RSTRING_LEN(separator));
    i_key_value_t *kv = new_key_value(key, sub_map);
    add_key_value(&key_map->data.hash, kv);
  }

  if (TYPE(key) == T_ARRAY)
    key = join_array_key(self, key, separator);
  else if (TYPE(key) != T_STRING)
    key = rb_funcall(key, s_to_s, 0);

  i_object_t *key_frd = hash_get(sub_map, &key, 1);

  if (key_frd == NULL) {
    char *sep = StringValueCStr(separator);
    VALUE parts = rb_str_split(key, sep);
    long parts_len = RARRAY_LEN(parts),
         skipped = 0;
    key_frd = new_array_object(parts_len);
    for (long i = 0; i < parts_len; i++) {
      VALUE part = RARRAY_PTR(parts)[i];
      // TODO: don't alloc for empty strings, since we discard them
      if (RSTRING_LEN(part) == 0)
        skipped++;
      else
        set_string_object(&key_frd->data.array[i - skipped], RSTRING_PTR(part), RSTRING_LEN(part));
    }
    key_frd->size -= skipped;

    char *key_orig = new_string(RSTRING_PTR(key), RSTRING_LEN(key));
    i_key_value_t *kv = new_key_value(key_orig, key_frd);
    add_key_value(&sub_map->data.hash, kv);
  }
  return i_object_to_robject(key_frd);
}

static VALUE
initialize(VALUE self)
{
  VALUE translations, key_cache;

  i_object_t *root_object = new_hash_object();
  translations = Data_Wrap_Struct(I18nemaBackend, 0, delete_object_r, root_object);
  rb_iv_set(self, "@translations", translations);

  i_object_t *key_map = new_hash_object();
  key_cache = Data_Wrap_Struct(I18nemaBackend, 0, delete_object_r, key_map);
  rb_iv_set(self, "@normalized_key_cache", key_cache);

  return self;
}

void
Init_i18nema()
{
  I18nema = rb_define_module("I18nema");
  I18nemaBackend = rb_define_class_under(I18nema, "Backend", rb_cObject);
  I18nemaBackendLoadError = rb_define_class_under(I18nemaBackend, "LoadError", rb_eStandardError);

  s_init_translations = rb_intern("init_translations");
  s_to_f = rb_intern("to_f");
  s_to_s = rb_intern("to_s");
  s_to_sym = rb_intern("to_sym");

  i_object_null.type = i_type_null;
  i_object_true.type = i_type_true;
  i_object_false.type = i_type_false;

  rb_define_method(I18nemaBackend, "initialize", initialize, 0);
  rb_define_method(I18nemaBackend, "load_yml_string", load_yml_string, 1);
  rb_define_method(I18nemaBackend, "available_locales", available_locales, 0);
  rb_define_method(I18nemaBackend, "reload!", reload, 0);
  rb_define_method(I18nemaBackend, "direct_lookup", direct_lookup, -1);
  rb_define_method(I18nemaBackend, "normalize_key", normalize_key, 2);
}

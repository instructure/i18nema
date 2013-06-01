#include <ruby.h>
#include <ruby/encoding.h>
#include "vendor/syck.h"
#include "vendor/uthash.h"

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

enum i_object_type {
  i_type_none,
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
    rb_ary_store(result, i, i_object_to_robject(array->data.array[i]));
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
  VALUE rs;
  char *s;

  for (int i = 0; i < argc && result != NULL && result->type == i_type_hash; i++) {
    rs = rb_funcall(argv[i], s_to_s, 0);
    s = StringValueCStr(rs);
    HASH_FIND_STR(result->data.hash, s, kv);
    result = kv == NULL ? NULL : kv->value;
  }

  return i_object_to_robject(result);
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
        delete_object_r(object->data.array[i]);
    xfree(object->data.array);
    break;
  case i_type_hash:
    delete_hash(&object->data.hash, recurse);
    break;
  case i_type_none:
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
  if (object->type != i_type_null && object->type != i_type_true && object->type != i_type_false)
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
  // key object whose string we have yoinked into a kv
  if (object->type == i_type_none)
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

static i_object_t*
new_string_object(char *str, long len)
{
  i_object_t *object = ALLOC(i_object_t);
  object->type = i_type_string;
  object->size = len;
  object->data.string = xmalloc(len + 1);
  strncpy(object->data.string, str, len);
  object->data.string[len] = '\0';
  return object;
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
    result = ALLOC(i_object_t);
    result->type = i_type_array;
    result->size = node->data.list->idx;
    result->data.array = ALLOC_N(i_object_t*, node->data.list->idx);
    for (long i = 0; i < node->data.list->idx; i++) {
      i_object_t *item = NULL;

      oid = syck_seq_read(node, i);
      syck_lookup_sym(parser, oid, (void **)&item);
      if (item->type == i_type_string)
        current_translation_count++;
      result->data.array[i] = item;
    }
    break;
  case syck_map_kind:
    result = ALLOC(i_object_t);
    result->type = i_type_hash;
    result->data.hash = NULL;
    for (long i = 0; i < node->data.pairs->idx; i++) {
      i_object_t *key = NULL, *value = NULL;

      oid = syck_map_read(node, map_key, i);
      syck_lookup_sym(parser, oid, (void **)&key);
      oid = syck_map_read(node, map_value, i);
      syck_lookup_sym(parser, oid, (void **)&value);

      i_key_value_t *kv;
      kv = ALLOC(i_key_value_t);
      kv->key = key->data.string;
      key->type = i_type_none; // so we know to free this node in delete_syck_st_entry
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
  empty_object(root_object, 1);
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
  translations = Data_Wrap_Struct(I18nemaBackend, 0, delete_object_r, root_object);
  rb_iv_set(self, "@translations", translations);
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
}

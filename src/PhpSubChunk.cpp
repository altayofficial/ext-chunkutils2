#include <new>

#include "PhpLightArray.h"
#include "PhpPalettedBlockArrayObj.h"
#include "PhpSubChunk.h"
#include "ZendUtil.h"
#include "stubs/pocketmine/world/format/SubChunk_arginfo.h"

extern "C" {
#include "php.h"
#include "ext/standard/php_var.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_gc.h"
#include "Zend/zend_hash.h"
#include "ext/spl/spl_exceptions.h"
}

#define SUB_CHUNK_COORD_BIT_SIZE 4
#define SUB_CHUNK_EDGE_LENGTH (1 << SUB_CHUNK_COORD_BIT_SIZE)
#define SUB_CHUNK_COORD_MASK (SUB_CHUNK_EDGE_LENGTH - 1)

typedef struct {
	zend_long emptyBlockId;
	paletted_block_array_obj* blockLayer0;
	paletted_block_array_obj* blockLayer1;
	paletted_block_array_obj* biomes;
	light_array_obj* skyLight;
	light_array_obj* blockLight;
	zend_object std;
} sub_chunk_obj;

zend_class_entry* sub_chunk_entry;
static zend_object_handlers sub_chunk_handlers;

static inline sub_chunk_obj* fetch_intern(zend_object* object) {
	return fetch_from_zend_object<sub_chunk_obj>(object);
}

/*
 * A SubChunk whose constructor never ran (subclass that doesn't call the parent constructor,
 * ReflectionClass::newInstanceWithoutConstructor()) has no biome array. Every method relies on it
 * being there, so such an object is refused instead of being dereferenced.
 */
static inline bool is_initialized(const sub_chunk_obj* object) {
	return object->biomes != nullptr;
}

static inline sub_chunk_obj* fetch_initialized(zend_object* object) {
	sub_chunk_obj* intern = fetch_intern(object);
	if (UNEXPECTED(!is_initialized(intern))) {
		zend_throw_error(NULL, "%s has not been initialized, its constructor was not called", ZSTR_VAL(object->ce->name));
		return nullptr;
	}
	return intern;
}

#define SUB_CHUNK_FETCH_THIS(intern) \
	sub_chunk_obj* intern = fetch_initialized(Z_OBJ_P(getThis())); \
	if (intern == nullptr) { \
		return; \
	}

/* owned object helpers */

template<typename object_type>
static object_type* take_owned_object(zval* zv) {
	if (zv == nullptr || Z_TYPE_P(zv) != IS_OBJECT) {
		return nullptr;
	}
	Z_ADDREF_P(zv);
	return fetch_from_zend_object<object_type>(Z_OBJ_P(zv));
}

template<typename object_type>
static void release_owned_object(object_type** slot) {
	if (*slot != nullptr) {
		object_type* previous = *slot;
		*slot = nullptr;
		OBJ_RELEASE(&previous->std);
	}
}

template<typename object_type>
static object_type* clone_owned_object(object_type* object) {
	if (object == nullptr) {
		return nullptr;
	}
	return fetch_from_zend_object<object_type>(object->std.handlers->clone_obj(&object->std));
}

template<typename object_type>
static void return_owned_object_or_null(zval* return_value, object_type* object) {
	if (object == nullptr) {
		RETURN_NULL();
	}
	RETURN_OBJ_COPY(&object->std);
}

/* internal object handlers */

static zend_object* sub_chunk_new(zend_class_entry* ce) {
	sub_chunk_obj* object = alloc_custom_zend_object<sub_chunk_obj>(ce, &sub_chunk_handlers);

	object->emptyBlockId = 0;
	object->blockLayer0 = nullptr;
	object->blockLayer1 = nullptr;
	object->biomes = nullptr;
	object->skyLight = nullptr;
	object->blockLight = nullptr;

	return &object->std;
}

static zend_object* sub_chunk_clone(zend_object* object) {
	sub_chunk_obj* const old_object = fetch_intern(object);
	sub_chunk_obj* const new_object = fetch_intern(sub_chunk_new(object->ce));

	new_object->emptyBlockId = old_object->emptyBlockId;
	new_object->blockLayer0 = clone_owned_object(old_object->blockLayer0);
	new_object->blockLayer1 = clone_owned_object(old_object->blockLayer1);
	new_object->biomes = clone_owned_object(old_object->biomes);
	new_object->skyLight = clone_owned_object(old_object->skyLight);
	new_object->blockLight = clone_owned_object(old_object->blockLight);

	zend_objects_clone_members(&new_object->std, &old_object->std); //copy user-assigned properties

	return &new_object->std;
}

static void sub_chunk_free(zend_object* std) {
	auto object = fetch_intern(std);

	release_owned_object(&object->blockLayer0);
	release_owned_object(&object->blockLayer1);
	release_owned_object(&object->biomes);
	release_owned_object(&object->skyLight);
	release_owned_object(&object->blockLight);

	zend_object_std_dtor(std);
}

//the owned arrays can't point back at anything, but dynamic properties can, so the cycle collector
//has to see both
template<typename object_type>
static void sub_chunk_gc_add(zend_get_gc_buffer* gc_buffer, object_type* object) {
	if (object != nullptr) {
		zval zv;
		ZVAL_OBJ(&zv, &object->std);
		zend_get_gc_buffer_add_zval(gc_buffer, &zv);
	}
}

static HashTable* sub_chunk_get_gc(zend_object* object, zval** table, int* n) {
	auto intern = fetch_intern(object);
	zend_get_gc_buffer* gc_buffer = zend_get_gc_buffer_create();

	sub_chunk_gc_add(gc_buffer, intern->blockLayer0);
	sub_chunk_gc_add(gc_buffer, intern->blockLayer1);
	sub_chunk_gc_add(gc_buffer, intern->biomes);
	sub_chunk_gc_add(gc_buffer, intern->skyLight);
	sub_chunk_gc_add(gc_buffer, intern->blockLight);

	zend_get_gc_buffer_use(gc_buffer, table, n);
	return zend_std_get_properties(object);
}

template<typename object_type>
static void sub_chunk_serialize_nullable(smart_str* buf, object_type* object, php_serialize_data_t* serialize_data) {
	zval zv;
	if (object != nullptr) {
		ZVAL_OBJ(&zv, &object->std);
	} else {
		ZVAL_NULL(&zv);
	}
	php_var_serialize(buf, &zv, serialize_data);
}

static int sub_chunk_serialize(zval* obj, unsigned char** buffer, size_t* buf_len, zend_serialize_data* data) {
	sub_chunk_obj* object = fetch_initialized(Z_OBJ_P(obj));
	if (object == nullptr) {
		return FAILURE;
	}

	smart_str buf = { 0 };
	php_serialize_data_t serialize_data = (php_serialize_data_t)data;
	PHP_VAR_SERIALIZE_INIT(serialize_data);

	zval zv;

	ZVAL_LONG(&zv, object->emptyBlockId);
	php_var_serialize(&buf, &zv, &serialize_data);

	sub_chunk_serialize_nullable(&buf, object->blockLayer0, &serialize_data);
	sub_chunk_serialize_nullable(&buf, object->blockLayer1, &serialize_data);
	sub_chunk_serialize_nullable(&buf, object->biomes, &serialize_data);
	sub_chunk_serialize_nullable(&buf, object->skyLight, &serialize_data);
	sub_chunk_serialize_nullable(&buf, object->blockLight, &serialize_data);

	ZVAL_ARR(&zv, zend_std_get_properties(Z_OBJ_P(obj)));
	php_var_serialize(&buf, &zv, &serialize_data);

	PHP_VAR_SERIALIZE_DESTROY(serialize_data);
	*buffer = (unsigned char*)estrndup(ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
	*buf_len = ZSTR_LEN(buf.s);
	zend_string_release(buf.s);

	return SUCCESS;
}

static inline bool is_instance_of(zval* zv, zend_class_entry* ce) {
	return Z_TYPE_P(zv) == IS_OBJECT && instanceof_function(Z_OBJCE_P(zv), ce);
}

static inline bool is_null_or_instance_of(zval* zv, zend_class_entry* ce) {
	return Z_TYPE_P(zv) == IS_NULL || is_instance_of(zv, ce);
}

static int sub_chunk_unserialize(zval* object, zend_class_entry* ce, const unsigned char* buf, size_t buf_len, zend_unserialize_data* data) {
	php_unserialize_data_t unserialize_data = (php_unserialize_data_t)data;
	PHP_VAR_UNSERIALIZE_INIT(unserialize_data);

	int result = FAILURE;

	const unsigned char* start = buf;
	const unsigned char* end = buf + buf_len;

	zval *emptyBlockId, *blockLayer0, *blockLayer1, *biomes, *skyLight, *blockLight, *properties;
	sub_chunk_obj* intern;

	emptyBlockId = var_tmp_var(&unserialize_data);
	if (!php_var_unserialize(emptyBlockId, &start, end, &unserialize_data) || Z_TYPE_P(emptyBlockId) != IS_LONG) {
		zend_throw_exception(NULL, "Failed to unserialize SubChunk::emptyBlockId", 0);
		goto end;
	}
	if (!paletted_block_array_check_entry_size(Z_LVAL_P(emptyBlockId), zend_ce_exception)) {
		goto end;
	}

	blockLayer0 = var_tmp_var(&unserialize_data);
	if (!php_var_unserialize(blockLayer0, &start, end, &unserialize_data) || !is_null_or_instance_of(blockLayer0, paletted_block_array_entry)) {
		zend_throw_exception(NULL, "Failed to unserialize SubChunk::blockLayer0", 0);
		goto end;
	}

	blockLayer1 = var_tmp_var(&unserialize_data);
	if (!php_var_unserialize(blockLayer1, &start, end, &unserialize_data) || !is_null_or_instance_of(blockLayer1, paletted_block_array_entry)) {
		zend_throw_exception(NULL, "Failed to unserialize SubChunk::blockLayer1", 0);
		goto end;
	}

	biomes = var_tmp_var(&unserialize_data);
	if (!php_var_unserialize(biomes, &start, end, &unserialize_data) || !is_instance_of(biomes, paletted_block_array_entry)) {
		zend_throw_exception(NULL, "Failed to unserialize SubChunk::biomes", 0);
		goto end;
	}

	skyLight = var_tmp_var(&unserialize_data);
	if (!php_var_unserialize(skyLight, &start, end, &unserialize_data) || !is_null_or_instance_of(skyLight, light_array_entry)) {
		zend_throw_exception(NULL, "Failed to unserialize SubChunk::skyLight", 0);
		goto end;
	}

	blockLight = var_tmp_var(&unserialize_data);
	if (!php_var_unserialize(blockLight, &start, end, &unserialize_data) || !is_null_or_instance_of(blockLight, light_array_entry)) {
		zend_throw_exception(NULL, "Failed to unserialize SubChunk::blockLight", 0);
		goto end;
	}

	object_init_ex(object, ce);
	intern = fetch_intern(Z_OBJ_P(object));
	intern->emptyBlockId = Z_LVAL_P(emptyBlockId);
	intern->blockLayer0 = take_owned_object<paletted_block_array_obj>(blockLayer0);
	intern->blockLayer1 = take_owned_object<paletted_block_array_obj>(blockLayer1);
	intern->biomes = take_owned_object<paletted_block_array_obj>(biomes);
	intern->skyLight = take_owned_object<light_array_obj>(skyLight);
	intern->blockLight = take_owned_object<light_array_obj>(blockLight);

	properties = var_tmp_var(&unserialize_data);
	if (!php_var_unserialize(properties, &start, end, &unserialize_data) || Z_TYPE_P(properties) != IS_ARRAY) {
		zend_throw_exception(NULL, "Failed to unserialize SubChunk user properties", 0);
		goto end;
	}
	if (zend_hash_num_elements(Z_ARRVAL_P(properties)) != 0) {
		zend_hash_copy(zend_std_get_properties(Z_OBJ_P(object)), Z_ARRVAL_P(properties), (copy_ctor_func_t)zval_add_ref);
	}
	result = SUCCESS;
end:
	PHP_VAR_UNSERIALIZE_DESTROY(unserialize_data);
	return result;
}

static void sub_chunk_gc_block_layer(paletted_block_array_obj** layer, zend_long emptyBlockId) {
	if (*layer == nullptr) {
		return;
	}
	(*layer)->container.collectGarbage(false);
	if ((*layer)->container.getBitsPerBlock() == VanillaPaletteSize::BPB_0 && (*layer)->container.get(0, 0, 0) == (Block)emptyBlockId) {
		release_owned_object(layer);
	}
}

static void sub_chunk_collect_garbage(sub_chunk_obj* object) {
	sub_chunk_gc_block_layer(&object->blockLayer0, object->emptyBlockId);
	sub_chunk_gc_block_layer(&object->blockLayer1, object->emptyBlockId);
	if (object->blockLayer0 == nullptr && object->blockLayer1 != nullptr) {
		object->blockLayer0 = object->blockLayer1;
		object->blockLayer1 = nullptr;
	}

	object->biomes->container.collectGarbage(false);

	if (object->skyLight != nullptr && object->skyLight->lightArray.isUniform(0)) {
		release_owned_object(&object->skyLight);
	}
	if (object->blockLight != nullptr && object->blockLight->lightArray.isUniform(0)) {
		release_owned_object(&object->blockLight);
	}
}

/* PHP-land SubChunk methods */
#define SUB_CHUNK_METHOD(name) PHP_METHOD(pocketmine_world_format_SubChunk, name)

SUB_CHUNK_METHOD(__construct) {
	zend_long emptyBlockId;
	zval* blockLayer0;
	zval* blockLayer1;
	zval* biomes;
	zval* skyLight = NULL;
	zval* blockLight = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 4, 6)
		Z_PARAM_LONG(emptyBlockId)
		Z_PARAM_OBJECT_OF_CLASS_EX(blockLayer0, paletted_block_array_entry, 1, 0)
		Z_PARAM_OBJECT_OF_CLASS_EX(blockLayer1, paletted_block_array_entry, 1, 0)
		Z_PARAM_OBJECT_OF_CLASS(biomes, paletted_block_array_entry)
		Z_PARAM_OPTIONAL
		Z_PARAM_OBJECT_OF_CLASS_EX(skyLight, light_array_entry, 1, 0)
		Z_PARAM_OBJECT_OF_CLASS_EX(blockLight, light_array_entry, 1, 0)
	ZEND_PARSE_PARAMETERS_END();

	auto intern = fetch_intern(Z_OBJ_P(getThis()));
	if (is_initialized(intern)) {
		zend_throw_error(NULL, "%s has already been initialized", ZSTR_VAL(Z_OBJCE_P(getThis())->name));
		return;
	}
	if (!paletted_block_array_check_entry_size(emptyBlockId, spl_ce_InvalidArgumentException)) {
		return;
	}

	intern->emptyBlockId = emptyBlockId;
	intern->blockLayer0 = take_owned_object<paletted_block_array_obj>(blockLayer0);
	intern->blockLayer1 = take_owned_object<paletted_block_array_obj>(blockLayer1);
	intern->biomes = take_owned_object<paletted_block_array_obj>(biomes);
	intern->skyLight = take_owned_object<light_array_obj>(skyLight);
	intern->blockLight = take_owned_object<light_array_obj>(blockLight);
}

SUB_CHUNK_METHOD(isEmptyAuthoritative) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	sub_chunk_collect_garbage(intern);
	RETURN_BOOL(intern->blockLayer0 == nullptr && intern->blockLayer1 == nullptr);
}

SUB_CHUNK_METHOD(isEmptyFast) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	RETURN_BOOL(intern->blockLayer0 == nullptr && intern->blockLayer1 == nullptr);
}

SUB_CHUNK_METHOD(getEmptyBlockId) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	RETURN_LONG(intern->emptyBlockId);
}

SUB_CHUNK_METHOD(getBlockStateId) {
	zend_long x, y, z;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 3, 3)
		Z_PARAM_LONG(x)
		Z_PARAM_LONG(y)
		Z_PARAM_LONG(z)
	ZEND_PARSE_PARAMETERS_END();

	SUB_CHUNK_FETCH_THIS(intern);
	if (intern->blockLayer0 == nullptr) {
		RETURN_LONG(intern->emptyBlockId);
	}
	RETURN_LONG(intern->blockLayer0->container.get(x, y, z));
}

SUB_CHUNK_METHOD(setBlockStateId) {
	zend_long x, y, z, block;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 4, 4)
		Z_PARAM_LONG(x)
		Z_PARAM_LONG(y)
		Z_PARAM_LONG(z)
		Z_PARAM_LONG(block)
	ZEND_PARSE_PARAMETERS_END();

	SUB_CHUNK_FETCH_THIS(intern);
	if (!paletted_block_array_check_entry_size(block, spl_ce_InvalidArgumentException)) {
		return;
	}

	if (intern->blockLayer0 == nullptr) {
		zval zvNewLayer;
		object_init_ex(&zvNewLayer, paletted_block_array_entry);
		auto newLayer = fetch_from_zend_object<paletted_block_array_obj>(Z_OBJ(zvNewLayer));
		if (!paletted_block_array_fill(newLayer, intern->emptyBlockId)) {
			zval_ptr_dtor(&zvNewLayer);
			return;
		}
		//ownership of the new object's only reference moves to the subchunk
		intern->blockLayer0 = newLayer;
	}
	intern->blockLayer0->container.set(x, y, z, block);
}

SUB_CHUNK_METHOD(getBlockLayer0) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	return_owned_object_or_null(return_value, intern->blockLayer0);
}

SUB_CHUNK_METHOD(getBlockLayer1) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	return_owned_object_or_null(return_value, intern->blockLayer1);
}

SUB_CHUNK_METHOD(getBlockLayers) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	if (intern->blockLayer0 == nullptr && intern->blockLayer1 == nullptr) {
		//most subchunks in a world are air, and the immutable empty array costs nothing to hand out
		RETURN_EMPTY_ARRAY();
	}

	array_init_size(return_value, 2);
	for (auto layer : {intern->blockLayer0, intern->blockLayer1}) {
		if (layer != nullptr) {
			zval zv;
			ZVAL_OBJ_COPY(&zv, &layer->std);
			add_next_index_zval(return_value, &zv);
		}
	}
}

SUB_CHUNK_METHOD(getHighestBlockAt) {
	zend_long x, z;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 2, 2)
		Z_PARAM_LONG(x)
		Z_PARAM_LONG(z)
	ZEND_PARSE_PARAMETERS_END();

	SUB_CHUNK_FETCH_THIS(intern);
	auto layerZero = intern->blockLayer0;
	if (layerZero == nullptr) {
		RETURN_NULL();
	}

	for (auto y = SUB_CHUNK_EDGE_LENGTH - 1; y >= 0; --y) {
		if (layerZero->container.get(x, y, z) != (Block)intern->emptyBlockId) {
			RETURN_LONG(y);
		}
	}
	RETURN_NULL(); //highest block not in this subchunk
}

SUB_CHUNK_METHOD(getBiomeArray) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	RETURN_OBJ_COPY(&intern->biomes->std);
}

static inline light_array_obj* ensure_light_array_initialized(light_array_obj** lightArray) {
	if (*lightArray == nullptr) {
		zval zv;
		object_init_ex(&zv, light_array_entry);
		auto intern = fetch_from_zend_object<light_array_obj>(Z_OBJ(zv));
		light_array_fill(intern, 0);
		*lightArray = intern;
	}
	return *lightArray;
}

SUB_CHUNK_METHOD(getBlockSkyLightArray) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	RETURN_OBJ_COPY(&ensure_light_array_initialized(&intern->skyLight)->std);
}

SUB_CHUNK_METHOD(getBlockLightArray) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	RETURN_OBJ_COPY(&ensure_light_array_initialized(&intern->blockLight)->std);
}

#define macro_SubChunk_setLightArray(lightArrayFieldName) \
	zval* data; \
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1) \
		Z_PARAM_OBJECT_OF_CLASS(data, light_array_entry) \
	ZEND_PARSE_PARAMETERS_END(); \
	SUB_CHUNK_FETCH_THIS(intern); \
	light_array_obj* const previous = intern->lightArrayFieldName; \
	intern->lightArrayFieldName = take_owned_object<light_array_obj>(data); \
	if (previous != nullptr) { \
		OBJ_RELEASE(&previous->std); \
	}

SUB_CHUNK_METHOD(setBlockSkyLightArray) {
	macro_SubChunk_setLightArray(skyLight);
}

SUB_CHUNK_METHOD(setBlockLightArray) {
	macro_SubChunk_setLightArray(blockLight);
}

SUB_CHUNK_METHOD(__debugInfo) {
	ZEND_PARSE_PARAMETERS_NONE();

	array_init(return_value);
}

SUB_CHUNK_METHOD(collectGarbage) {
	ZEND_PARSE_PARAMETERS_NONE();

	SUB_CHUNK_FETCH_THIS(intern);
	sub_chunk_collect_garbage(intern);
}

void register_sub_chunk_class() {
	memcpy(&sub_chunk_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	sub_chunk_handlers.offset = XtOffsetOf(sub_chunk_obj, std);
	sub_chunk_handlers.free_obj = sub_chunk_free;
	sub_chunk_handlers.clone_obj = sub_chunk_clone;
	sub_chunk_handlers.get_gc = sub_chunk_get_gc;

	zend_class_entry ce;
	INIT_CLASS_ENTRY(ce, "pocketmine\\world\\format\\SubChunk", class_pocketmine_world_format_SubChunk_methods);
	ce.create_object = sub_chunk_new;
	ce.serialize = sub_chunk_serialize;
	ce.unserialize = sub_chunk_unserialize;
	sub_chunk_entry = zend_register_internal_class(&ce);

	zend_declare_class_constant_long(sub_chunk_entry, "COORD_BIT_SIZE", sizeof("COORD_BIT_SIZE") - 1, SUB_CHUNK_COORD_BIT_SIZE);
	zend_declare_class_constant_long(sub_chunk_entry, "COORD_MASK", sizeof("COORD_MASK") - 1, SUB_CHUNK_COORD_MASK);
	zend_declare_class_constant_long(sub_chunk_entry, "EDGE_LENGTH", sizeof("EDGE_LENGTH") - 1, SUB_CHUNK_EDGE_LENGTH);
}

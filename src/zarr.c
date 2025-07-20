#include "vcr.h"



static chunk* zarr_decompress_chunk(s32 size, void* compressed_data, zarrinfo metadata) {
    if(strcmp(metadata.dtype,"|u1") != 0) {
        LOG_ERROR("unsupported zarr format. Only u8 is supported\n");
    }

    u8* decompressed_data = malloc(CHUNK_LEN*CHUNK_LEN*CHUNK_LEN);
    int decompressed_size = blosc2_decompress(compressed_data, size, decompressed_data, CHUNK_LEN*CHUNK_LEN*CHUNK_LEN);
    if (decompressed_size < 0) {
        LOG_ERROR("Blosc2 decompression failed: %d\n", decompressed_size);
        free(decompressed_data);
        return nullptr;
    }
    chunk *ret = chunk_new();

    for (int z = 0; z < CHUNK_LEN; z++) {
        for (int y = 0; y < CHUNK_LEN; y++) {
            for (int x = 0; x < CHUNK_LEN; x++) {
                (*ret)[z][y][x] = decompressed_data[z * CHUNK_LEN * CHUNK_LEN + y * CHUNK_LEN + x];
            }
        }
    }
    free(decompressed_data);
    return ret;
}

chunk* zarr_read_chunk(char* path, zarrinfo metadata) {

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERROR("Failed to open chunk file: %s\n", path);
        return NULL;
    }
    
    fseek(fp, 0, SEEK_END);
    s32 size = (s32)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    u8* compressed_data = malloc(size);
    fread(compressed_data,1,size,fp);
    fclose(fp);  // Close the file!

    chunk* ret= zarr_decompress_chunk(size, compressed_data, metadata);
    free(compressed_data);
    return ret;
}


zarrinfo zarr_parse_zarray(const char* json_string) {
    zarrinfo info = {0}; // Initialize all fields to 0

    struct json_value_s* root = json_parse(json_string, strlen(json_string));
    if (!root) {
        printf("Failed to parse JSON\n");
        return info;
    }

    struct json_object_s* root_object = json_value_as_object(root);
    if (!root_object) {
        printf("Root is not an object\n");
        free(root);
        return info;
    }

    struct json_object_element_s* element = root_object->start;

    while (element) {
        const char* key = element->name->string;
        struct json_value_s* value = element->value;

        if (strcmp(key, "chunks") == 0) {
            struct json_array_s* chunks_array = json_value_as_array(value);
            if (chunks_array) {
                struct json_array_element_s* chunk_elem = chunks_array->start;
                int i = 0;
                while (chunk_elem && i < 3) {
                    struct json_number_s* num = json_value_as_number(chunk_elem->value);
                    if (num) {
                        info.chunks[i] = atoi(num->number);
                    }
                    chunk_elem = chunk_elem->next;
                    i++;
                }
            }
        }
        else if (strcmp(key, "compressor") == 0) {
            struct json_object_s* compressor = json_value_as_object(value);
            if (compressor) {
                struct json_object_element_s* comp_elem = compressor->start;
                while (comp_elem) {
                    const char* comp_key = comp_elem->name->string;

                    if (strcmp(comp_key, "blocksize") == 0) {
                        struct json_number_s* num = json_value_as_number(comp_elem->value);
                        if (num) info.compressor.blocksize = atoi(num->number);
                    }
                    else if (strcmp(comp_key, "clevel") == 0) {
                        struct json_number_s* num = json_value_as_number(comp_elem->value);
                        if (num) info.compressor.clevel = atoi(num->number);
                    }
                    else if (strcmp(comp_key, "cname") == 0) {
                        struct json_string_s* str = json_value_as_string(comp_elem->value);
                        if (str) {
                            strncpy(info.compressor.cname, str->string, 31);
                            info.compressor.cname[31] = '\0';
                        }
                    }
                    else if (strcmp(comp_key, "id") == 0) {
                        struct json_string_s* str = json_value_as_string(comp_elem->value);
                        if (str) {
                            strncpy(info.compressor.id, str->string, 31);
                            info.compressor.id[31] = '\0';
                        }
                    }
                    else if (strcmp(comp_key, "shuffle") == 0) {
                        struct json_number_s* num = json_value_as_number(comp_elem->value);
                        if (num) info.compressor.shuffle = atoi(num->number);
                    }

                    comp_elem = comp_elem->next;
                }
            }
        }
        else if (strcmp(key, "dimension_separator") == 0) {
            struct json_string_s* str = json_value_as_string(value);
            if (str && str->string[0]) {
                info.dimension_separator = str->string[0];
            }
        }
        else if (strcmp(key, "dtype") == 0) {
            struct json_string_s* str = json_value_as_string(value);
            if (str) {
                strncpy(info.dtype, str->string, 31);
                info.dtype[31] = '\0';
            }
        }
        else if (strcmp(key, "fill_value") == 0) {
            struct json_number_s* num = json_value_as_number(value);
            if (num) {
                info.fill_value = atoi(num->number);
            }
        }
        else if (strcmp(key, "filters") == 0) {
            if (json_value_is_null(value)) {
                info.filters = NULL;
            }
        }
        else if (strcmp(key, "order") == 0) {
            struct json_string_s* str = json_value_as_string(value);
            if (str && str->string[0]) {
                info.order = str->string[0];
            }
        }
        else if (strcmp(key, "shape") == 0) {
            struct json_array_s* shape_array = json_value_as_array(value);
            if (shape_array) {
                struct json_array_element_s* shape_elem = shape_array->start;
                int i = 0;
                while (shape_elem && i < 3) {
                    struct json_number_s* num = json_value_as_number(shape_elem->value);
                    if (num) {
                        info.shape[i] = atoi(num->number);
                    }
                    shape_elem = shape_elem->next;
                    i++;
                }
            }
        }
        else if (strcmp(key, "zarr_format") == 0) {
            struct json_number_s* num = json_value_as_number(value);
            if (num) {
                info.zarr_format = atoi(num->number);
            }
        }
        element = element->next;
    }

    free(root);

    return info;
}
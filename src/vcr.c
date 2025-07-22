#include "vcr.h"
#include <stdio.h>

// Sokol headers
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_log.h"
#include "sokol_glue.h"

// Nuklear configuration and headers
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_VARARGS
#include "nuklear.h"
#define SOKOL_METAL
#include "sokol_nuklear.h"
#include "sokol_gl.h"


// Application state
typedef struct {
    zarrinfo zarr_info;
    char info_text[1024];
    char zarr_path[512];
    
    // Chunk loading parameters
    s32 chunk_offset[3]; // z, y, x
    s32 chunk_size[3];   // z, y, x
    
    // Loaded volume data
    volume* loaded_volume;
    chunk* loaded_chunk;  // Keep for single chunk mode
    s32 current_slice[3]; // z, y, x indices for the current position
    
    // Textures for displaying slices
    sg_image slice_images[3]; // XY, XZ, YZ views
    snk_image_t snk_imgs[3];
    bool slice_images_created[3];
    
    // Which view is currently active for keyboard navigation
    int active_view; // 0=XY, 1=XZ, 2=YZ
    
    // 3D rendering state
    sgl_context sgl_ctx_3d;
    sgl_pipeline sgl_pip_3d;
    sgl_pipeline sgl_pip_transparent;  // Pipeline for transparent objects
    
    // Mesh data from marching cubes
    mesh* chunk_meshes;  // Array of meshes, one per chunk in volume
    int num_chunk_meshes;
    mesh current_mesh;  // Keep for single chunk mode
    float rotation_x, rotation_y;
    u8 iso_threshold;  // Threshold for isosurface
    
    // Render target for 3D view
    sg_image render_target_3d;
    sg_image depth_target_3d;
    sg_attachments attachments_3d;
    snk_image_t snk_render_target_3d;
    bool render_3d_created;
} app_state_t;

static app_state_t app_state;


// Helper to get voxel value from either chunk or volume
static u8 get_voxel_value(int z, int y, int x) {
    if (app_state.loaded_volume) {
        // Get from volume
        int chunk_z = z / CHUNK_LEN;
        int chunk_y = y / CHUNK_LEN;
        int chunk_x = x / CHUNK_LEN;
        
        if (chunk_z >= app_state.loaded_volume->z || 
            chunk_y >= app_state.loaded_volume->y || 
            chunk_x >= app_state.loaded_volume->x ||
            chunk_z < 0 || chunk_y < 0 || chunk_x < 0) {
            return 0;
        }
        
        int local_z = z % CHUNK_LEN;
        int local_y = y % CHUNK_LEN;
        int local_x = x % CHUNK_LEN;
        
        int chunk_idx = chunk_z * app_state.loaded_volume->y * app_state.loaded_volume->x + 
                       chunk_y * app_state.loaded_volume->x + chunk_x;
        return app_state.loaded_volume->chunks[chunk_idx][local_z][local_y][local_x];
    } else if (app_state.loaded_chunk) {
        // Get from single chunk
        if (z >= 0 && z < CHUNK_LEN && y >= 0 && y < CHUNK_LEN && x >= 0 && x < CHUNK_LEN) {
            return (*app_state.loaded_chunk)[z][y][x];
        }
    }
    return 0;
}

// Create or update a specific slice texture
static void update_slice_texture(int view_idx) {
    if (!app_state.loaded_chunk && !app_state.loaded_volume) return;
    
    // Determine texture size based on volume or chunk
    int tex_size = CHUNK_LEN;
    if (app_state.loaded_volume) {
        // For volume, create texture large enough for all chunks
        switch (view_idx) {
            case 0: // XY view
                tex_size = fmax(app_state.loaded_volume->y * CHUNK_LEN, 
                               app_state.loaded_volume->x * CHUNK_LEN);
                break;
            case 1: // XZ view
                tex_size = fmax(app_state.loaded_volume->z * CHUNK_LEN,
                               app_state.loaded_volume->x * CHUNK_LEN);
                break;
            case 2: // YZ view
                tex_size = fmax(app_state.loaded_volume->z * CHUNK_LEN,
                               app_state.loaded_volume->y * CHUNK_LEN);
                break;
        }
    }
    
    // Create RGBA data
    u8* rgba_data = calloc(tex_size * tex_size * 4, 1);
    
    // Extract the appropriate slice based on view type
    for (int i = 0; i < tex_size; i++) {
        for (int j = 0; j < tex_size; j++) {
            u8 gray = 0;
            int z_coord = 0, y_coord = 0, x_coord = 0;
            
            switch (view_idx) {
                case 0: // XY view (constant Z)
                    z_coord = app_state.current_slice[0];
                    y_coord = i;
                    x_coord = j;
                    break;
                case 1: // XZ view (constant Y)
                    z_coord = i;
                    y_coord = app_state.current_slice[1];
                    x_coord = j;
                    break;
                case 2: // YZ view (constant X)
                    z_coord = i;
                    y_coord = j;
                    x_coord = app_state.current_slice[2];
                    break;
            }
            
            // Get voxel value
            gray = get_voxel_value(z_coord, y_coord, x_coord);
            
            int idx = i * tex_size + j;
            
            // Add crosshairs to show current position
            bool is_crosshair = false;
            switch (view_idx) {
                case 0: // XY view - show Y and X position
                    is_crosshair = (i == app_state.current_slice[1] || j == app_state.current_slice[2]);
                    break;
                case 1: // XZ view - show Z and X position  
                    is_crosshair = (i == app_state.current_slice[0] || j == app_state.current_slice[2]);
                    break;
                case 2: // YZ view - show Z and Y position
                    is_crosshair = (i == app_state.current_slice[0] || j == app_state.current_slice[1]);
                    break;
            }
            
            if (is_crosshair) {
                // Red crosshairs
                rgba_data[idx * 4 + 0] = 255;  // R
                rgba_data[idx * 4 + 1] = gray / 2;  // G
                rgba_data[idx * 4 + 2] = gray / 2;  // B
            } else {
                rgba_data[idx * 4 + 0] = gray;  // R
                rgba_data[idx * 4 + 1] = gray;  // G
                rgba_data[idx * 4 + 2] = gray;  // B
            }
            rgba_data[idx * 4 + 3] = 255;   // A (fully opaque)
        }
    }
    
    // Destroy existing image if any
    if (app_state.slice_images_created[view_idx]) {
        snk_destroy_image(app_state.snk_imgs[view_idx]);
        sg_destroy_image(app_state.slice_images[view_idx]);
    }
    
    // Create new image
    app_state.slice_images[view_idx] = sg_make_image(&(sg_image_desc){
        .width = tex_size,
        .height = tex_size,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.subimage[0][0] = {
            .ptr = rgba_data,
            .size = tex_size * tex_size * 4
        }
    });
    
    // Create sokol-nuklear image wrapper
    app_state.snk_imgs[view_idx] = snk_make_image(&(snk_image_desc_t){
        .image = app_state.slice_images[view_idx],
        // sampler is optional, will use default
    });
    
    app_state.slice_images_created[view_idx] = true;
    
    free(rgba_data);
}

// Update all slice textures
static void update_all_slice_textures(void) {
    for (int i = 0; i < 3; i++) {
        update_slice_texture(i);
    }
}

// Load chunk from zarr
static void load_chunk(void) {
    if (strlen(app_state.zarr_path) == 0 || app_state.zarr_info.zarr_format == 0) {
        sprintf(app_state.info_text, "Please load a Zarr array first");
        return;
    }
    
    // Validate chunk alignment
    for (int i = 0; i < 3; i++) {
        if (app_state.chunk_offset[i] % CHUNK_LEN != 0) {
            sprintf(app_state.info_text, "Chunk offset must be aligned to %d", CHUNK_LEN);
            return;
        }
        if (app_state.chunk_size[i] != CHUNK_LEN) {
            sprintf(app_state.info_text, "Chunk size must be %d", CHUNK_LEN);
            return;
        }
    }
    
    // Free previous chunk if any
    if (app_state.loaded_chunk) {
        chunk_free(app_state.loaded_chunk);
        app_state.loaded_chunk = NULL;
    }
    
    // Construct chunk path using dimension separator
    char chunk_path[1024];
    char sep = app_state.zarr_info.dimension_separator;
    
    // Default to '.' if separator is not set
    if (sep == '\0') {
        sep = '.';
    }
    
    snprintf(chunk_path, sizeof(chunk_path), "%s/%d%c%d%c%d", 
             app_state.zarr_path,
             app_state.chunk_offset[0] / CHUNK_LEN,
             sep,
             app_state.chunk_offset[1] / CHUNK_LEN,
             sep,
             app_state.chunk_offset[2] / CHUNK_LEN);
    
    // Load the chunk
    app_state.loaded_chunk = zarr_read_chunk(chunk_path, app_state.zarr_info);
    
    if (app_state.loaded_chunk) {
        sprintf(app_state.info_text, "Successfully loaded chunk from: %s", chunk_path);
        // Initialize to center of chunk
        app_state.current_slice[0] = CHUNK_LEN / 2; // Z
        app_state.current_slice[1] = CHUNK_LEN / 2; // Y
        app_state.current_slice[2] = CHUNK_LEN / 2; // X
        app_state.active_view = 0; // Start with XY view
        update_all_slice_textures();
        
        // Generate 3D mesh
        mesh_free(&app_state.current_mesh);
        app_state.current_mesh = generate_mesh_from_chunk(app_state.loaded_chunk, app_state.iso_threshold);
    } else {
        sprintf(app_state.info_text, "Failed to load chunk from: %s", chunk_path);
    }
}

// Function to load volume from zarr array
static void load_volume(void) {
    if (!app_state.zarr_info.chunks[0]) {
        sprintf(app_state.info_text, "Please load a zarr array first");
        return;
    }
    
    // For now, load a 2x2x2 volume of chunks
    s32 volume_size[3] = {2, 2, 2};  // z, y, x chunks
    
    // Free previous volume if any
    if (app_state.loaded_volume) {
        volume_free(app_state.loaded_volume);
        app_state.loaded_volume = NULL;
    }
    
    // Free previous meshes
    if (app_state.chunk_meshes) {
        for (int i = 0; i < app_state.num_chunk_meshes; i++) {
            mesh_free(&app_state.chunk_meshes[i]);
        }
        free(app_state.chunk_meshes);
        app_state.chunk_meshes = NULL;
    }
    
    // Load the volume
    app_state.loaded_volume = zarr_read_volume(app_state.zarr_path, app_state.zarr_info,
                                               app_state.chunk_offset[0] / CHUNK_LEN, 
                                               app_state.chunk_offset[1] / CHUNK_LEN, 
                                               app_state.chunk_offset[2] / CHUNK_LEN,
                                               volume_size[0], volume_size[1], volume_size[2]);
    
    if (app_state.loaded_volume) {
        sprintf(app_state.info_text, "Successfully loaded %dx%dx%d volume from offset [%d,%d,%d]", 
                volume_size[0], volume_size[1], volume_size[2],
                app_state.chunk_offset[0], app_state.chunk_offset[1], app_state.chunk_offset[2]);
        
        // Generate meshes for each chunk
        int total_chunks = volume_size[0] * volume_size[1] * volume_size[2];
        app_state.chunk_meshes = malloc(total_chunks * sizeof(mesh));
        app_state.num_chunk_meshes = 0;
        
        for (int z = 0; z < volume_size[0]; z++) {
            for (int y = 0; y < volume_size[1]; y++) {
                for (int x = 0; x < volume_size[2]; x++) {
                    int idx = z * volume_size[1] * volume_size[2] + y * volume_size[2] + x;
                    app_state.chunk_meshes[app_state.num_chunk_meshes] = 
                        generate_mesh_from_chunk(&app_state.loaded_volume->chunks[idx], app_state.iso_threshold);
                    
                    // Offset the mesh vertices to position the chunk correctly
                    mesh* m = &app_state.chunk_meshes[app_state.num_chunk_meshes];
                    if (m->vertices && m->num_triangles > 0) {
                        for (int i = 0; i < m->num_triangles * 3; i++) {
                            m->vertices[i * 3 + 0] += x * CHUNK_LEN;  // X offset
                            m->vertices[i * 3 + 1] += y * CHUNK_LEN;  // Y offset
                            m->vertices[i * 3 + 2] += z * CHUNK_LEN;  // Z offset
                        }
                        app_state.num_chunk_meshes++;
                    }
                }
            }
        }
        
        LOG_INFO("Generated %d meshes from volume\n", app_state.num_chunk_meshes);
        
        // Initialize slice position to center of volume
        app_state.current_slice[0] = (volume_size[0] * CHUNK_LEN) / 2;
        app_state.current_slice[1] = (volume_size[1] * CHUNK_LEN) / 2;
        app_state.current_slice[2] = (volume_size[2] * CHUNK_LEN) / 2;
        
        // Update slice textures
        update_all_slice_textures();
    } else {
        sprintf(app_state.info_text, "Failed to load volume");
    }
}

// Function to load and parse .zarray file from a zarr volume path
static void load_zarr_array(const char* zarr_path) {
    char zarray_path[1024];
    snprintf(zarray_path, sizeof(zarray_path), "%s/.zarray", zarr_path);

    char* json_content = read_file(zarray_path);
    if (json_content) {
        app_state.zarr_info = zarr_parse_zarray(json_content);
        snprintf(app_state.info_text, sizeof(app_state.info_text),
                 "Successfully loaded .zarray from: %s", zarray_path);
        free(json_content);
    } else {
        snprintf(app_state.info_text, sizeof(app_state.info_text),
                 "Failed to load .zarray from: %s", zarray_path);
        memset(&app_state.zarr_info, 0, sizeof(zarrinfo));
    }
}

static void init(void) {
    // Initialize app state first
    memset(&app_state, 0, sizeof(app_state));
    sprintf(app_state.info_text, "Volume Cartographer Reloaded Ready");
    strcpy(app_state.zarr_path, "");
    
    // Initialize chunk parameters
    for (int i = 0; i < 3; i++) {
        app_state.chunk_offset[i] = 1024;  // Default offset into zarr volume
        app_state.chunk_size[i] = CHUNK_LEN;
        app_state.current_slice[i] = 0;
        app_state.slice_images_created[i] = false;
    }
    app_state.active_view = 0;
    app_state.iso_threshold = 128;  // Default threshold
    app_state.rotation_x = 0.0f;
    app_state.rotation_y = 0.0f;
    
    // Setup sokol-gfx
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    // Setup sokol-nuklear integration
    snk_setup(&(snk_desc_t){
        .enable_set_mouse_cursor = true,
        .dpi_scale = sapp_dpi_scale(),
        .logger.func = slog_func,
    });
    
    // Setup sokol-gl with larger vertex buffer for marching cubes
    sgl_setup(&(sgl_desc_t){
        .max_vertices = 8 * 1024 * 1024,  // 8M vertices (enough for multiple chunks)
        .logger.func = slog_func,
    });
    
    // Create render target for 3D view
    const int rt_size = 512;
    app_state.render_target_3d = sg_make_image(&(sg_image_desc){
        .usage.render_attachment = true,
        .width = rt_size,
        .height = rt_size,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
    });
    app_state.depth_target_3d = sg_make_image(&(sg_image_desc){
        .usage.render_attachment = true,
        .width = rt_size,
        .height = rt_size,
        .pixel_format = SG_PIXELFORMAT_DEPTH,
    });
    
    // Create attachments for render target
    app_state.attachments_3d = sg_make_attachments(&(sg_attachments_desc){
        .colors[0].image = app_state.render_target_3d,
        .depth_stencil.image = app_state.depth_target_3d,
    });
    
    // Create sokol-gl context for 3D rendering with large vertex buffer
    app_state.sgl_ctx_3d = sgl_make_context(&(sgl_context_desc_t){
        .max_vertices = 8 * 1024 * 1024,  // Match the main context size
        .max_commands = 16 * 1024,
        .color_format = SG_PIXELFORMAT_RGBA8,
        .depth_format = SG_PIXELFORMAT_DEPTH,
        .sample_count = 1,
    });
    
    // Create pipeline for 3D rendering
    app_state.sgl_pip_3d = sgl_context_make_pipeline(app_state.sgl_ctx_3d, &(sg_pipeline_desc){
        .cull_mode = SG_CULLMODE_NONE,  // Disable culling to see both sides
        .depth = {
            .write_enabled = true,
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
        },
    });
    
    // Create pipeline for transparent objects (slice planes)
    app_state.sgl_pip_transparent = sgl_context_make_pipeline(app_state.sgl_ctx_3d, &(sg_pipeline_desc){
        .cull_mode = SG_CULLMODE_NONE,
        .depth = {
            .write_enabled = false,  // Don't write to depth buffer for transparent objects
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
        },
        .colors[0] = {
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            }
        }
    });
    
    // Create sokol-nuklear image wrapper for render target
    app_state.snk_render_target_3d = snk_make_image(&(snk_image_desc_t){
        .image = app_state.render_target_3d,
        .sampler = sg_make_sampler(&(sg_sampler_desc){
            .min_filter = SG_FILTER_LINEAR,
            .mag_filter = SG_FILTER_LINEAR,
        })
    });
    app_state.render_3d_created = true;
}

// Helper function to render a mesh with lighting
static void render_mesh_with_lighting(mesh* m, float light_dir[3]) {
    if (!m || !m->vertices || m->num_triangles <= 0) return;
    
    sgl_begin_triangles();
    
    for (int i = 0; i < m->num_triangles; i++) {
        float* v = &m->vertices[i * 9];
        float* c = &m->colors[i * 9];
        
        // Calculate face normal using cross product
        float v1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]};
        float v2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
        
        float normal[3];
        normal[0] = v1[1] * v2[2] - v1[2] * v2[1];
        normal[1] = v1[2] * v2[0] - v1[0] * v2[2];
        normal[2] = v1[0] * v2[1] - v1[1] * v2[0];
        
        // Normalize the normal
        float normal_len = sqrtf(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
        if (normal_len > 0.0001f) {
            normal[0] /= normal_len;
            normal[1] /= normal_len;
            normal[2] /= normal_len;
        }
        
        // Calculate lighting (dot product between normal and light direction)
        float dot = -(normal[0] * light_dir[0] + normal[1] * light_dir[1] + normal[2] * light_dir[2]);
        dot = fmaxf(0.0f, dot); // Clamp to positive values
        
        // Apply lighting with ambient component
        float ambient = 0.5f;  // Increased from 0.3f for brighter scene
        float diffuse = 0.6f;  // Slightly reduced to compensate for higher ambient
        float lighting = ambient + diffuse * dot;
        
        // Apply lighting to vertex colors
        // First vertex
        sgl_c3f(c[0] * lighting, c[1] * lighting, c[2] * lighting);
        sgl_v3f(v[0], v[1], v[2]);
        
        // Second vertex
        sgl_c3f(c[3] * lighting, c[4] * lighting, c[5] * lighting);
        sgl_v3f(v[3], v[4], v[5]);
        
        // Third vertex
        sgl_c3f(c[6] * lighting, c[7] * lighting, c[8] * lighting);
        sgl_v3f(v[6], v[7], v[8]);
    }
    
    sgl_end();
}

// Render 3D view to texture
static void render_3d_view(void) {
    if ((!app_state.loaded_chunk && !app_state.loaded_volume) || !app_state.render_3d_created) return;
    
    // Set the sokol-gl context
    sgl_set_context(app_state.sgl_ctx_3d);
    sgl_defaults();
    sgl_load_pipeline(app_state.sgl_pip_3d);
    
    // Setup 3D projection
    sgl_matrix_mode_projection();
    sgl_perspective(sgl_rad(45.0f), 1.0f, 0.1f, 1000.0f);
    
    // Setup camera - adjust for volume vs single chunk
    sgl_matrix_mode_modelview();
    
    float center_x = 64.0f, center_y = 64.0f, center_z = 64.0f;
    float eye_dist = 200.0f;
    
    if (app_state.loaded_volume) {
        // For volume, center on the middle of the volume
        center_x = app_state.loaded_volume->x * CHUNK_LEN / 2.0f;
        center_y = app_state.loaded_volume->y * CHUNK_LEN / 2.0f;
        center_z = app_state.loaded_volume->z * CHUNK_LEN / 2.0f;
        eye_dist = fmaxf(app_state.loaded_volume->x, fmaxf(app_state.loaded_volume->y, app_state.loaded_volume->z)) * CHUNK_LEN * 1.5f;
    }
    
    sgl_lookat(center_x + eye_dist, center_y + eye_dist, center_z + eye_dist,  // eye
               center_x, center_y, center_z,      // center
               0.0f, 1.0f, 0.0f);                 // up
    
    // Apply rotation
    sgl_translate(center_x, center_y, center_z);
    sgl_rotate(sgl_rad(app_state.rotation_x), 1.0f, 0.0f, 0.0f);
    sgl_rotate(sgl_rad(app_state.rotation_y), 0.0f, 1.0f, 0.0f);
    sgl_translate(-center_x, -center_y, -center_z);
    
    // Fixed light direction (pointing down and slightly forward)
    float light_dir[3] = {0.0f, -0.8f, -0.6f};
    // Normalize light direction
    float light_len = sqrtf(light_dir[0]*light_dir[0] + light_dir[1]*light_dir[1] + light_dir[2]*light_dir[2]);
    light_dir[0] /= light_len;
    light_dir[1] /= light_len;
    light_dir[2] /= light_len;
    
    // Draw meshes - either volume meshes or single chunk mesh
    if (app_state.chunk_meshes && app_state.num_chunk_meshes > 0) {
        // Render all chunk meshes in the volume
        for (int i = 0; i < app_state.num_chunk_meshes; i++) {
            render_mesh_with_lighting(&app_state.chunk_meshes[i], light_dir);
        }
    } else if (app_state.current_mesh.vertices && app_state.current_mesh.num_triangles > 0) {
        // Render single chunk mesh
        render_mesh_with_lighting(&app_state.current_mesh, light_dir);
    }
    
    // Draw slice planes as semi-transparent quads
    if (app_state.loaded_chunk || app_state.loaded_volume) {
        // Switch to transparent pipeline
        sgl_load_pipeline(app_state.sgl_pip_transparent);
        
        float x = (float)app_state.current_slice[2];
        float y = (float)app_state.current_slice[1];
        float z = (float)app_state.current_slice[0];
        
        // Determine the size based on whether we have a volume or single chunk
        float max_x = CHUNK_LEN;
        float max_y = CHUNK_LEN;
        float max_z = CHUNK_LEN;
        
        if (app_state.loaded_volume) {
            max_x = app_state.loaded_volume->x * CHUNK_LEN;
            max_y = app_state.loaded_volume->y * CHUNK_LEN;
            max_z = app_state.loaded_volume->z * CHUNK_LEN;
        }
        
        // XY plane (constant Z) - blue tint
        sgl_begin_quads();
        sgl_c4f(0.2f, 0.3f, 0.8f, 0.5f);  // Increased alpha from 0.3f to 0.5f
        sgl_v3f(0.0f, 0.0f, z);
        sgl_v3f(max_x, 0.0f, z);
        sgl_v3f(max_x, max_y, z);
        sgl_v3f(0.0f, max_y, z);
        sgl_end();
        
        // XZ plane (constant Y) - green tint
        sgl_begin_quads();
        sgl_c4f(0.2f, 0.8f, 0.3f, 0.5f);  // Increased alpha from 0.3f to 0.5f
        sgl_v3f(0.0f, y, 0.0f);
        sgl_v3f(max_x, y, 0.0f);
        sgl_v3f(max_x, y, max_z);
        sgl_v3f(0.0f, y, max_z);
        sgl_end();
        
        // YZ plane (constant X) - red tint
        sgl_begin_quads();
        sgl_c4f(0.8f, 0.2f, 0.3f, 0.5f);  // Increased alpha from 0.3f to 0.5f
        sgl_v3f(x, 0.0f, 0.0f);
        sgl_v3f(x, max_y, 0.0f);
        sgl_v3f(x, max_y, max_z);
        sgl_v3f(x, 0.0f, max_z);
        sgl_end();
        
        // Draw intersection lines where planes meet (more opaque)
        sgl_begin_lines();
        sgl_c4f(1.0f, 1.0f, 0.0f, 0.8f);  // Yellow, more opaque
        
        // XY-XZ intersection (along X axis at y,z)
        sgl_v3f(0.0f, y, z);
        sgl_v3f(max_x, y, z);
        
        // XY-YZ intersection (along Y axis at x,z)
        sgl_v3f(x, 0.0f, z);
        sgl_v3f(x, max_y, z);
        
        // XZ-YZ intersection (along Z axis at x,y)
        sgl_v3f(x, y, 0.0f);
        sgl_v3f(x, y, max_z);
        sgl_end();
        
        // Draw intersection point as a small sphere (using a cube for simplicity)
        sgl_begin_quads();
        sgl_c4f(1.0f, 1.0f, 1.0f, 0.9f);  // White, nearly opaque
        float s = 2.0f; // Size of the intersection marker
        
        // Front face
        sgl_v3f(x-s, y-s, z+s); sgl_v3f(x+s, y-s, z+s);
        sgl_v3f(x+s, y+s, z+s); sgl_v3f(x-s, y+s, z+s);
        // Back face
        sgl_v3f(x-s, y-s, z-s); sgl_v3f(x+s, y-s, z-s);
        sgl_v3f(x+s, y+s, z-s); sgl_v3f(x-s, y+s, z-s);
        // Top face
        sgl_v3f(x-s, y+s, z-s); sgl_v3f(x+s, y+s, z-s);
        sgl_v3f(x+s, y+s, z+s); sgl_v3f(x-s, y+s, z+s);
        // Bottom face
        sgl_v3f(x-s, y-s, z-s); sgl_v3f(x+s, y-s, z-s);
        sgl_v3f(x+s, y-s, z+s); sgl_v3f(x-s, y-s, z+s);
        // Right face
        sgl_v3f(x+s, y-s, z-s); sgl_v3f(x+s, y-s, z+s);
        sgl_v3f(x+s, y+s, z+s); sgl_v3f(x+s, y+s, z-s);
        // Left face
        sgl_v3f(x-s, y-s, z-s); sgl_v3f(x-s, y-s, z+s);
        sgl_v3f(x-s, y+s, z+s); sgl_v3f(x-s, y+s, z-s);
        sgl_end();
        
        // Restore the original pipeline for solid objects
        sgl_load_pipeline(app_state.sgl_pip_3d);
    }
    
    // Draw coordinate axes for reference
    float axis_len = app_state.loaded_volume ? 
        fmaxf(app_state.loaded_volume->x, fmaxf(app_state.loaded_volume->y, app_state.loaded_volume->z)) * CHUNK_LEN :
        CHUNK_LEN;
    
    sgl_begin_lines();
    // X axis - red
    sgl_c3f(1.0f, 0.0f, 0.0f);
    sgl_v3f(0.0f, 0.0f, 0.0f);
    sgl_v3f(axis_len, 0.0f, 0.0f);
    // Y axis - green
    sgl_c3f(0.0f, 1.0f, 0.0f);
    sgl_v3f(0.0f, 0.0f, 0.0f);
    sgl_v3f(0.0f, axis_len, 0.0f);
    // Z axis - blue
    sgl_c3f(0.0f, 0.0f, 1.0f);
    sgl_v3f(0.0f, 0.0f, 0.0f);
    sgl_v3f(0.0f, 0.0f, axis_len);
    sgl_end();
}

// Helper function to draw a slice viewer window
void draw_slice_viewer(struct nk_context *ctx, const char* title, int view_idx, float x, float y) {
    const char* axis_names[3] = {"Z", "Y", "X"};
    const char* view_names[3] = {"XY", "XZ", "YZ"};

    if (nk_begin(ctx, title, nk_rect(x, y, 300, 350),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                 NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {

        // Highlight active window
        if (app_state.active_view == view_idx) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "[ACTIVE - Use arrows to navigate]", NK_TEXT_CENTERED);
        }

        // Display current slice info
        char buffer[256];
        int max_val = CHUNK_LEN - 1;
        if (app_state.loaded_volume) {
            switch (view_idx) {
                case 0: max_val = app_state.loaded_volume->z * CHUNK_LEN - 1; break;
                case 1: max_val = app_state.loaded_volume->y * CHUNK_LEN - 1; break;
                case 2: max_val = app_state.loaded_volume->x * CHUNK_LEN - 1; break;
            }
        }
        sprintf(buffer, "%s View - %s: %d / %d",
                view_names[view_idx],
                axis_names[view_idx],
                app_state.current_slice[view_idx],
                max_val);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, buffer, NK_TEXT_CENTERED);

        // Make window clickable to set as active
        struct nk_rect bounds = nk_window_get_bounds(ctx);
        if (nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, bounds)) {
            app_state.active_view = view_idx;
        }

        // Display the image with aspect ratio maintained
        if (app_state.slice_images_created[view_idx]) {
            // Get the available content area
            struct nk_rect content_bounds = nk_window_get_content_region(ctx);
            float available_width = content_bounds.w;
            float available_height = content_bounds.h - 60; // Account for the labels above

            // Calculate the size to maintain square aspect ratio
            float size = (available_width < available_height) ? available_width : available_height;

            // Center the image if there's extra space
            if (available_width > size) {
                float padding = (available_width - size) / 2.0f;
                nk_layout_row_begin(ctx, NK_STATIC, size, 3);
                nk_layout_row_push(ctx, padding);
                nk_spacing(ctx, 1);
                nk_layout_row_push(ctx, size);
                nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_imgs[view_idx])));
                nk_layout_row_end(ctx);
            } else {
                nk_layout_row_dynamic(ctx, size, 1);
                nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_imgs[view_idx])));
            }
        }
    }
    nk_end(ctx);
}

static void frame(void) {
    // Start new Nuklear frame
    struct nk_context *ctx = snk_new_frame();

    // Main window
    if (nk_begin(ctx, "Volume Cartographer Reloaded", nk_rect(10, 10, 500, 750),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | 
                 NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {
        
        // Menu bar
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 25, 2);
        nk_layout_row_push(ctx, 45);
        if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(120, 200))) {
            nk_layout_row_dynamic(ctx, 25, 1);
            if (nk_menu_item_label(ctx, "Clear", NK_TEXT_LEFT)) {
                memset(&app_state.zarr_info, 0, sizeof(zarrinfo));
                strcpy(app_state.zarr_path, "");
                sprintf(app_state.info_text, "Cleared");
                
                // Clean up chunk data
                if (app_state.loaded_chunk) {
                    chunk_free(app_state.loaded_chunk);
                    app_state.loaded_chunk = NULL;
                }
                
                // Reset chunk parameters
                for (int i = 0; i < 3; i++) {
                    app_state.chunk_offset[i] = 0;
                    app_state.chunk_size[i] = CHUNK_LEN;
                    app_state.current_slice[i] = 0;
                }
                app_state.active_view = 0;
            }
            if (nk_menu_item_label(ctx, "Exit", NK_TEXT_LEFT)) {
                sapp_request_quit();
            }
            nk_menu_end(ctx);
        }
        nk_menubar_end(ctx);

        // Zarr path input section
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "Zarr Volume Path:", NK_TEXT_LEFT);
        
        // Show hint text if path is empty
        if (strlen(app_state.zarr_path) == 0) {
            nk_layout_row_dynamic(ctx, 15, 1);
            nk_label(ctx, "(Enter path to directory containing .zarray file)", NK_TEXT_LEFT);
        }
        
        nk_layout_row_dynamic(ctx, 35, 1);
        // Use NK_EDIT_SIMPLE for better text editing behavior
        nk_flags evt = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER | NK_EDIT_CLIPBOARD, 
                                                       app_state.zarr_path, sizeof(app_state.zarr_path), 
                                                       nk_filter_default);
        
        // Load button or Enter key pressed
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Load Zarr Array") || (evt & NK_EDIT_COMMITED)) {
            if (strlen(app_state.zarr_path) > 0) {
                load_zarr_array(app_state.zarr_path);
            } else {
                sprintf(app_state.info_text, "Please enter a Zarr volume path");
            }
        }

        // Info section
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "Information:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 60, 1);
        nk_label_wrap(ctx, app_state.info_text);

        // Zarr Info Display
        if (app_state.zarr_info.zarr_format > 0) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Zarr Array Info:", NK_TEXT_LEFT);
            
            char buffer[256];
            sprintf(buffer, "Format: %d", app_state.zarr_info.zarr_format);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
            
            sprintf(buffer, "Shape: [%d, %d, %d]", 
                    app_state.zarr_info.shape[0], 
                    app_state.zarr_info.shape[1], 
                    app_state.zarr_info.shape[2]);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
            
            sprintf(buffer, "Chunks: [%d, %d, %d]", 
                    app_state.zarr_info.chunks[0], 
                    app_state.zarr_info.chunks[1], 
                    app_state.zarr_info.chunks[2]);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
            
            sprintf(buffer, "Data Type: %s", app_state.zarr_info.dtype);
            nk_label(ctx, buffer, NK_TEXT_LEFT);
            
            if (app_state.zarr_info.compressor.id[0]) {
                sprintf(buffer, "Compressor: %s (level %d)", 
                        app_state.zarr_info.compressor.id, 
                        app_state.zarr_info.compressor.clevel);
                nk_label(ctx, buffer, NK_TEXT_LEFT);
            }
            
            // Chunk loading section
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "", NK_TEXT_LEFT); // Spacing
            nk_label(ctx, "Load Chunk:", NK_TEXT_LEFT);
            
            // Offset inputs
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Offset (Z, Y, X):", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 30, 3);
            nk_property_int(ctx, "Z", 0, &app_state.chunk_offset[0], 10000, CHUNK_LEN, 1);
            nk_property_int(ctx, "Y", 0, &app_state.chunk_offset[1], 10000, CHUNK_LEN, 1);
            nk_property_int(ctx, "X", 0, &app_state.chunk_offset[2], 10000, CHUNK_LEN, 1);
            
            // Size inputs (fixed to CHUNK_LEN but shown for clarity)
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Size (must be 128):", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 30, 3);
            nk_property_int(ctx, "Z", CHUNK_LEN, &app_state.chunk_size[0], CHUNK_LEN, CHUNK_LEN, 0);
            nk_property_int(ctx, "Y", CHUNK_LEN, &app_state.chunk_size[1], CHUNK_LEN, CHUNK_LEN, 0);
            nk_property_int(ctx, "X", CHUNK_LEN, &app_state.chunk_size[2], CHUNK_LEN, CHUNK_LEN, 0);
            
            // Load chunk button
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "Load Chunk")) {
                load_chunk();
            }
            
            // Load volume button (2x2x2 chunks)
            if (nk_button_label(ctx, "Load Volume (2x2x2)")) {
                load_volume();
            }
        }
    }
    nk_end(ctx);

    
    // Draw all three slice viewers
    if (app_state.loaded_chunk || app_state.loaded_volume) {
        draw_slice_viewer(ctx, "XY Slice Viewer", 0, 520, 10);
        draw_slice_viewer(ctx, "XZ Slice Viewer", 1, 830, 10);
        draw_slice_viewer(ctx, "YZ Slice Viewer", 2, 520, 370);
        
        // Draw 3D viewer
        if (nk_begin(ctx, "3D View", nk_rect(830, 370, 300, 400),
                     NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | 
                     NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {
            
            // Iso threshold control
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Iso Threshold (0-255):", NK_TEXT_LEFT);
            
            nk_layout_row_dynamic(ctx, 25, 1);
            int threshold = (int)app_state.iso_threshold;
            nk_property_int(ctx, "##threshold", 0, &threshold, 255, 1, 5);
            app_state.iso_threshold = (u8)threshold;
            
            // Regenerate mesh button
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "Regenerate Mesh")) {
                if (app_state.loaded_volume) {
                    // Regenerate all volume meshes
                    if (app_state.chunk_meshes) {
                        for (int i = 0; i < app_state.num_chunk_meshes; i++) {
                            mesh_free(&app_state.chunk_meshes[i]);
                        }
                        free(app_state.chunk_meshes);
                        app_state.chunk_meshes = NULL;
                        app_state.num_chunk_meshes = 0;
                    }
                    
                    // Generate new meshes
                    int total_chunks = app_state.loaded_volume->z * app_state.loaded_volume->y * app_state.loaded_volume->x;
                    app_state.chunk_meshes = malloc(total_chunks * sizeof(mesh));
                    app_state.num_chunk_meshes = 0;
                    
                    for (int z = 0; z < app_state.loaded_volume->z; z++) {
                        for (int y = 0; y < app_state.loaded_volume->y; y++) {
                            for (int x = 0; x < app_state.loaded_volume->x; x++) {
                                int idx = z * app_state.loaded_volume->y * app_state.loaded_volume->x + 
                                         y * app_state.loaded_volume->x + x;
                                app_state.chunk_meshes[app_state.num_chunk_meshes] = 
                                    generate_mesh_from_chunk(&app_state.loaded_volume->chunks[idx], app_state.iso_threshold);
                                
                                // Offset the mesh vertices
                                mesh* m = &app_state.chunk_meshes[app_state.num_chunk_meshes];
                                if (m->vertices && m->num_triangles > 0) {
                                    for (int i = 0; i < m->num_triangles * 3; i++) {
                                        m->vertices[i * 3 + 0] += x * CHUNK_LEN;
                                        m->vertices[i * 3 + 1] += y * CHUNK_LEN;
                                        m->vertices[i * 3 + 2] += z * CHUNK_LEN;
                                    }
                                    app_state.num_chunk_meshes++;
                                }
                            }
                        }
                    }
                } else if (app_state.loaded_chunk) {
                    // Regenerate single chunk mesh
                    mesh_free(&app_state.current_mesh);
                    app_state.current_mesh = generate_mesh_from_chunk(app_state.loaded_chunk, app_state.iso_threshold);
                }
            }
            
            // Rotation controls
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Drag to rotate", NK_TEXT_CENTERED);
            
            // Display 3D render target
            if (app_state.render_3d_created) {
                // Get the available content area
                struct nk_rect content_bounds = nk_window_get_content_region(ctx);
                float available_width = content_bounds.w;
                float available_height = content_bounds.h - 80; // Account for controls
                
                // Calculate the size to maintain square aspect ratio
                float size = (available_width < available_height) ? available_width : available_height;
                
                // Get mouse input for rotation
                struct nk_rect image_rect = nk_rect(
                    content_bounds.x + (available_width - size) / 2,
                    content_bounds.y + 80,
                    size, size
                );
                
                if (nk_input_is_mouse_hovering_rect(&ctx->input, image_rect)) {
                    if (ctx->input.mouse.buttons[NK_BUTTON_LEFT].down) {
                        app_state.rotation_y += ctx->input.mouse.delta.x * 0.5f;
                        app_state.rotation_x += ctx->input.mouse.delta.y * 0.5f;
                    }
                }
                
                // Center the image if there's extra space
                if (available_width > size) {
                    float padding = (available_width - size) / 2.0f;
                    nk_layout_row_begin(ctx, NK_STATIC, size, 3);
                    nk_layout_row_push(ctx, padding);
                    nk_spacing(ctx, 1);
                    nk_layout_row_push(ctx, size);
                    nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_render_target_3d)));
                    nk_layout_row_end(ctx);
                } else {
                    nk_layout_row_dynamic(ctx, size, 1);
                    nk_image(ctx, nk_image_handle(snk_nkhandle(app_state.snk_render_target_3d)));
                }
            }
        }
        nk_end(ctx);
    }

    // Render 3D view to texture first
    if ((app_state.loaded_chunk || app_state.loaded_volume) && app_state.render_3d_created) {
        render_3d_view();
        
        // Render to texture
        sg_begin_pass(&(sg_pass){
            .action = {
                .colors[0] = {
                    .load_action = SG_LOADACTION_CLEAR,
                    .clear_value = { 0.2f, 0.2f, 0.2f, 1.0f }  // Brightened background
                }
            },
            .attachments = app_state.attachments_3d
        });
        sgl_context_draw(app_state.sgl_ctx_3d);
        sg_end_pass();
        
        // Check for any sokol-gl errors
        sgl_error_t err = sgl_context_error(app_state.sgl_ctx_3d);
        if (err.any) {
            printf("sokol-gl error: vertices_full=%d, commands_full=%d\n", 
                   err.vertices_full, err.commands_full);
        }
    }
    
    // Main render pass
    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { 0.2f, 0.3f, 0.3f, 1.0f }
            }
        },
        .swapchain = sglue_swapchain()
    });
    snk_render(sapp_width(), sapp_height());
    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    // Clean up loaded chunk
    if (app_state.loaded_chunk) {
        chunk_free(app_state.loaded_chunk);
    }
    
    // Clean up loaded volume
    if (app_state.loaded_volume) {
        volume_free(app_state.loaded_volume);
    }
    
    // Clean up chunk meshes
    if (app_state.chunk_meshes) {
        for (int i = 0; i < app_state.num_chunk_meshes; i++) {
            mesh_free(&app_state.chunk_meshes[i]);
        }
        free(app_state.chunk_meshes);
    }
    
    // Clean up single mesh
    mesh_free(&app_state.current_mesh);
    
    // Clean up textures
    for (int i = 0; i < 3; i++) {
        if (app_state.slice_images_created[i]) {
            snk_destroy_image(app_state.snk_imgs[i]);
            sg_destroy_image(app_state.slice_images[i]);
        }
    }
    
    // Clean up 3D rendering resources
    if (app_state.render_3d_created) {
        snk_destroy_image(app_state.snk_render_target_3d);
        sg_destroy_attachments(app_state.attachments_3d);
        sg_destroy_image(app_state.render_target_3d);
        sg_destroy_image(app_state.depth_target_3d);
        sgl_destroy_context(app_state.sgl_ctx_3d);
    }
    
    sgl_shutdown();
    snk_shutdown();
    sg_shutdown();
}

static void input(const sapp_event* event) {
    // Handle slice navigation based on active view
    if (event->type == SAPP_EVENTTYPE_KEY_DOWN && (app_state.loaded_chunk || app_state.loaded_volume)) {
        bool update_needed = false;
        
        // Determine max slice based on volume or chunk
        int max_slice = CHUNK_LEN;
        if (app_state.loaded_volume) {
            switch (app_state.active_view) {
                case 0: max_slice = app_state.loaded_volume->z * CHUNK_LEN; break;
                case 1: max_slice = app_state.loaded_volume->y * CHUNK_LEN; break;
                case 2: max_slice = app_state.loaded_volume->x * CHUNK_LEN; break;
            }
        }
        
        if (event->key_code == SAPP_KEYCODE_RIGHT) {
            // Go to next slice (wrap around)
            app_state.current_slice[app_state.active_view] = 
                (app_state.current_slice[app_state.active_view] + 1) % max_slice;
            update_needed = true;
        } else if (event->key_code == SAPP_KEYCODE_LEFT) {
            // Go to previous slice (wrap around)
            app_state.current_slice[app_state.active_view] = 
                (app_state.current_slice[app_state.active_view] - 1 + max_slice) % max_slice;
            update_needed = true;
        } else if (event->key_code == SAPP_KEYCODE_TAB) {
            // Tab to cycle through views
            app_state.active_view = (app_state.active_view + 1) % 3;
        }
        
        if (update_needed) {
            // Update all views since they're connected
            update_all_slice_textures();
        }
    }
    
    snk_handle_event(event);
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc) {
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = input,
        .enable_clipboard = true,
        .width = 1200,
        .height = 800,
        .window_title = "Volume Cartographer Reloaded",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}



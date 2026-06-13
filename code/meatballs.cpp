#define _CRT_SECURE_NO_WARNINGS 1
#include "../include/raylib.h"
#include "../include/raymath.h"
#include "../include/rlgl.h"
#define RAYGUI_IMPLEMENTATION
#include "../include/raygui.h"
#define RLIGHTS_IMPLEMENTATION
#include "../include/rlights.h"

//#if defined(PLATFORM_DESKTOP)
    #define GLSL_VERSION            330
//#else   // PLATFORM_ANDROID, PLATFORM_WEB
  //  #define GLSL_VERSION            100
//#endif

#include "stdlib.h"
#include "stdint.h"
#include "math.h"
#include "stdio.h"

#include "intersection_edges_table.h"

#define FACE_NORMAL_LIGHTING 1
#define FLT_MAX     340282346638528859811704183484516925440.0f

#define internal static

#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))
#define Assert(Expression) if (!(Expression)) *(int *)0 = 0;
#define memory_index size_t

#define MemoryZero(p,size)    memset((p),0,(size))
#define MemoryZeroStruct(s)   MemoryZero((s),sizeof(*(s)))
#define MemoryZeroArray(a)    MemoryZero((a),sizeof(a))
#define MemoryZeroTyped(p,c)  MemoryZero((p),sizeof(*(p))*(c))

#define Kilobytes(Value) ((Value)*1024ULL)
#define Megabytes(Value) (Kilobytes(Value)*1024ULL)
#define Gigabytes(Value) (Megabytes(Value)*1024ULL)
#define Terabytes(Value) (Gigabytes(Value)*1024ULL)

#define PROFILE_START(name) \
    double start_##name = GetTime(); double end_##name = 0; double elapsed_##name = 0;

#define PROFILE_END(name) \
    end_##name = GetTime(); \
    elapsed_##name = (double)(end_##name - start_##name); \
    printf("[%s] Time: %.5f seconds\n", #name, elapsed_##name);

#define PROFILE_PAUSE(name, timer) \
    end_##name = GetTime(); \
    elapsed_##name = (double)(end_##name - start_##name); \
	timers[timer] += elapsed_##name;

#define PROFILE_TIMER(name, timer) \
    printf("[%s] Time: %.5f seconds\n", #name, timers[timer]);

struct memory_arena
{
	memory_index Size;
	memory_index Used;
	uint8_t *Base;
};

internal void
InitializeArena(memory_arena *Arena, memory_index Size, uint8_t *Base)
{
	Arena->Size = Size;
	Arena->Base = Base;
	Arena->Used = 0;
}

#define PushStruct(Arena, type) ((type *)PushSize_(Arena, sizeof(type)))
#define PushArray(Arena, Count, type) ((type *)PushSize_(Arena, (Count)*sizeof(type)))
internal void *
PushSize_(memory_arena *Arena, memory_index Size)
{
	Assert((Arena->Used + Size) <= Arena->Size);
	void *Result = Arena->Base + Arena->Used;
	Arena->Used += Size;

	return Result;
}

struct program_memory
{
	bool IsInitialized;
	uint64_t PermanentStorageSize;
	void *PermanentStorage; // NOTE(casey): REQUIRED to be cleared to zero at startup
							
	uint64_t TransientStorageSize;
	void *TransientStorage; // NOTE(casey): REQUIRED to be cleared to zero at startup

	//debug_platform_read_entire_file *DEBUGPlatformReadEntireFile;
	//debug_platform_free_file_memory *DEBUGPlatformFreeFileMemory;
	//debug_platform_write_entire_file *DEBUGPlatformWriteEntireFile;
};

static program_memory global_program_memory;

int global_draw_surfaces_points_count = 0;
bool global_draw_coordinate_axis = true;
bool global_draw_octree_wires = true;
bool global_draw_octree_grid = true;
bool global_draw_polygon_wires = true;
bool global_draw_backing_sphere = true;
bool global_flat_shading = false;

static Camera3D camera;
static Shader shader;
static bool global_shader_valid;
static uint64_t global_shader_vs_mod_time;
static uint64_t global_shader_fs_mod_time;


struct Size
{
	int Width;
	int Height;
};

struct VertexBuffers
{
	Vector3 *vertex;
	Vector3 *normal;
};

struct Vector3Node
{
	Vector3 key;
	VertexBuffers value;
	Vector3Node *next;
};

// https://nullprogram.com/blog/2018/07/31/
uint32_t murmurhash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x85ebca6bU;
    x ^= x >> 13;
    x *= 0xc2b2ae35U;
    x ^= x >> 16;
    return x;
}

// TODO: research on float hashing
// https://www.boost.org/doc/libs/1_35_0/doc/html/boost/hash_combine_id241013.html
uint32_t Vector3Hash(Vector3 v)
{
	uint32_t seed = 0;
    seed ^= *((uint32_t *)&v.x) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= *((uint32_t *)&v.y) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= *((uint32_t *)&v.z) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	return seed;
}

// https://gist.github.com/badboy/6267743
/*
uint32_t hash6432shift(uint64_t key)
{
  key = (~key) + (key << 18); // key = (key << 18) - key - 1;
  key = key ^ (key >>> 31);
  key = key * 21; // key = (key + (key << 2)) + (key << 4);
  key = key ^ (key >>> 11);
  key = key + (key << 6);
  key = key ^ (key >>> 22);
  return (int) key;
}
*/

#define BUCKET_COUNT 1000

struct hash_map
{
	void **buckets;
};

void hash_map_init(hash_map *map, memory_arena *arena)
{
	map->buckets = (void **)PushArray(arena, BUCKET_COUNT, Vector3Node *);
	MemoryZeroTyped((Vector3Node **)map->buckets, BUCKET_COUNT);
}

Vector3Node *hash_map_add(hash_map *map, memory_arena *arena, Vector3 key, VertexBuffers value)
{
	uint32_t hash = Vector3Hash(key);
	int bucket = hash % BUCKET_COUNT;

	Vector3Node **node = ((Vector3Node **)map->buckets) + bucket;
	while (*node != NULL) 
	{
		if (Vector3Equals((*node)->key, key))
		{
			(*node)->value = value;
			return *node;
		}
		node = &(*node)->next;
	}

	*node = PushStruct(arena, Vector3Node);
	MemoryZeroStruct(*node);
	(*node)->key = key;
	(*node)->value = value;
	return *node;
}

Vector3Node *hash_map_get(hash_map *map, Vector3 key)
{
	uint32_t hash = Vector3Hash(key);
	int bucket = hash % BUCKET_COUNT;

	Vector3Node *node = ((Vector3Node **)map->buckets)[bucket];
	while (node != NULL && !Vector3Equals(node->key, key)) 
	{
		node = node->next;
	}
	return node;
}

void ToggleMinimizeMaximize()
{
	if (IsKeyPressed(KEY_F))
	{
		if (IsWindowMaximized())
			MinimizeWindow();	
		else
			MaximizeWindow();
	}
}

inline float
Square(float A)
{
	float Result = A*A;
	return Result;
}

struct MetaSphere
{
	Vector3 center;
	float radius;
	int flags;
	Color color;
};

enum MB_OJECT_FLAGS
{
	MB_OBJECT_SELECTED = 1,
};

static float THRESHOLD = 0.2f;
float MetaSphereForceInPoint(Vector3 point, MetaSphere *metaspheres, int count)
{
	//NOTE: Positive inside sphere, negative outside
	
	//float force = Square(metasphere.radius) - Vector3DistanceSqr(point, metasphere.center);
	//float force = metasphere.radius - Vector3Distance(point, metasphere.center);
	
	float result = 0;
	for (int i = 0; i < count; i++)
	{
		float force = metaspheres[i].radius - Vector3Distance(point, metaspheres[i].center);
		if (force > 0)
			result += force;
	}

	return result;
}

Vector3 MetaSphereNormalInPoint(Vector3 point, MetaSphere *metaspheres, int count)
{
	float threshold_fix = 0;
	if (THRESHOLD < 0.15)
	{
		threshold_fix = 0.2f - THRESHOLD;
	}

	Vector3 result = {0};
	for (int i = 0; i < count; i++)
	{
		Vector3 sphere_to_point = Vector3Subtract(point, metaspheres[i].center);
		float length = (float)fmax(0, Square(metaspheres[i].radius + threshold_fix) - Vector3LengthSqr(sphere_to_point));
		result = Vector3Add(result, Vector3Scale(sphere_to_point, length));
	}
	return result;
}

struct OctreeVertex
{
	Vector3 point;
	float force;
	bool hot;
};

struct OctreeCube
{
	OctreeVertex *vertices[8];
};

struct OctreeEdge
{
	uint8_t v1;
	uint8_t v2;
};

static double timers[16] = {0};

#define OCTREE_RESOLUTION 1
#define OCTREE_CUBES 8
#define OCTREE_AXIS_POINTS 3
void MetaSphereMeshWithOctree(MetaSphere *metaspheres, int group_objects_count, Mesh *mesh, 
							  memory_arena *mesh_arena, memory_arena *normals_arena, memory_arena *indices_arena, memory_arena *map_arena, hash_map *vertex_map,
							  float axis_step, Vector3 start_point)
{
	//OctreeVertex* octree_vertices = (OctreeVertex *)malloc(sizeof(OctreeVertex)*3*3*3);
	OctreeVertex octree_vertices[3*3*3] = {0};
	OctreeCube cubes[OCTREE_CUBES] = {0};

	Vector3 octree_point = start_point;
	for (int zn = 0; zn < OCTREE_AXIS_POINTS; zn++)
	{
		octree_point.y = start_point.y;
		for (int yn = 0; yn < OCTREE_AXIS_POINTS; yn++)
		{
			octree_point.x = start_point.x;
			for (int xn = 0; xn < OCTREE_AXIS_POINTS; xn++)
			{
				PROFILE_START(compute_force);
				float force = MetaSphereForceInPoint(octree_point, metaspheres, group_objects_count);
				octree_vertices[((zn)*OCTREE_AXIS_POINTS + yn)*OCTREE_AXIS_POINTS+xn] = {octree_point, force, force > THRESHOLD};
				PROFILE_PAUSE(compute_force, 0);

				//if (force > 0)
				//	refine = true;

				octree_point.x += axis_step;
			}
			octree_point.y += axis_step;
		}
		octree_point.z += axis_step;
	}

PROFILE_START(collect_hot);
	int cube_origins[8] = {0, 1, 3, 4, 9, 10, 12, 13};
	int vertex_edge_offsets[6] = {1, 2, 4, -1, -2, -4};
	for (int i = 0; i < 8; i++)
	{
		OctreeCube *cube = &cubes[i];
		int origin = cube_origins[i];
		
		int hot_count = 0;

		uint8_t hot_vertices = 0;
		for (int j = 0; j < 8; j++)
		{
			cube->vertices[j] = &octree_vertices[origin + cube_origins[j]];
			if (cube->vertices[j]->hot)
			{
				hot_count++;
				hot_vertices |= 1 << j;
			}
		}

		if (hot_count == 0 || hot_count == 8)
		{
			continue;
		}
		
		static OctreeEdge edge_vertices[12] = {
			{0, 1}, {1, 3}, {2, 3}, {0, 2},
			{0, 4}, {1, 5}, {3, 7}, {2, 6},
			{4, 5}, {5, 7}, {6, 7}, {4, 6}
		};
		uint8_t *intersections = intersection_edges_from_hot_vertices[hot_vertices];
		Vector3 estimated_vertices[12] = {0};
		int vertex_count = 0;
	PROFILE_PAUSE(collect_hot,1);

	PROFILE_START(compute_vertex);
		for (int int_index = 0; int_index < 12; ++int_index)
		{
			// TODO: Document or rewrite clearer
			uint8_t edge_index = intersections[int_index];
			if (edge_index == 0xFF)
				break;
			vertex_count++;

			OctreeVertex *v1 = cube->vertices[edge_vertices[edge_index].v1];
			OctreeVertex *v2 = cube->vertices[edge_vertices[edge_index].v2];

			//t = (THRESHOLD - Force at B) / (Force at A - Force at B)
			//P = B*(1-t) + A*t;
			float t = (THRESHOLD - v2->force) / (v1->force - v2->force);
			estimated_vertices[int_index] = Vector3Add(Vector3Scale(v2->point, 1-t), Vector3Scale(v1->point, t));
		}
	PROFILE_PAUSE(compute_vertex,2);

	PROFILE_START(fill_buffers);
		if ((global_draw_surfaces_points_count - 2) * 3 == vertex_count || global_draw_surfaces_points_count == 0)
		{
#if FACE_NORMAL_LIGHTING
			bool face_normal_lighting = group_objects_count > 10;
#endif
			mesh->triangleCount += vertex_count / 3;
			for (int face_index = 0; face_index < vertex_count / 3; ++face_index)
			{
				Vector3 face_normal = Vector3Normalize(Vector3CrossProduct(
					Vector3Subtract(estimated_vertices[face_index*3+2], estimated_vertices[face_index*3+1]),
					Vector3Subtract(estimated_vertices[face_index*3+0], estimated_vertices[face_index*3+1])));
				for (int vertex_index = face_index*3; vertex_index < face_index*3 + 3; ++vertex_index)
				{
					Vector3Node *node = hash_map_get(vertex_map, estimated_vertices[vertex_index]);
					if (node == NULL)
					{
						Vector3 *mesh_vertex = (Vector3*)PushArray(mesh_arena, 1, Vector3);
						Vector3 *vertex_normal = (Vector3*)PushArray(normals_arena, 1, Vector3);

						*mesh_vertex = estimated_vertices[vertex_index];
						// TODO: compute isosurface normal for more than 2 surfaces correctly, vertex faces for now
#if FACE_NORMAL_LIGHTING
						if (face_normal_lighting)
							*vertex_normal = face_normal;
						else
#endif
							*vertex_normal = MetaSphereNormalInPoint(estimated_vertices[vertex_index], metaspheres, group_objects_count);
						mesh->vertexCount++;
						node = hash_map_add(vertex_map, map_arena, estimated_vertices[vertex_index], {mesh_vertex, vertex_normal});
					}
					else
					{
#if FACE_NORMAL_LIGHTING
						if (face_normal_lighting)
							*(node->value.normal) = Vector3Add(*(node->value.normal), face_normal);
#endif
					}
					uint16_t *vertex_index_buf = (uint16_t *)PushStruct(indices_arena, uint16_t);
					*vertex_index_buf = (uint16_t)(node->value.vertex - (Vector3 *)mesh_arena->Base);
				}
			}

			if (global_draw_octree_wires)
			{
				Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
				DrawCubeWires(draw_origin, axis_step, axis_step, axis_step, GREEN);
			}
			if (global_draw_octree_grid)
			{
				Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
				draw_origin.y = 0;
				if (CheckCollisionBoxes({cube->vertices[0]->point, cube->vertices[7]->point}, {camera.position, camera.position}))
				{
					for (int draw_vertex = 0; draw_vertex < 8; draw_vertex++)
					{
						DrawSphere(cube->vertices[draw_vertex]->point, 0.02, CLITERAL(Color){ 255, 255, 255, 155 });

						for (int vertex_index = 0; vertex_index+2 < vertex_count; vertex_index+=3)
						{
							DrawTriangle3D(estimated_vertices[vertex_index], estimated_vertices[vertex_index+1],estimated_vertices[vertex_index+2], WHITE);
						}
					}
				}
				DrawCubeWires(draw_origin, axis_step, 0, axis_step, GREEN);
				//TraceLog(LOG_INFO, "x:%.2f y:%.2f", camera.position.x, camera.position.y);

				draw_origin = cube->vertices[0]->point;
				draw_origin.y = 0;
				draw_origin.x -= axis_step/2;
				Vector3 draw_end = draw_origin;
				draw_end.x += axis_step*2;
				DrawLine3D(draw_origin, draw_end, GREEN);

				draw_origin.z += axis_step;
				draw_end.z += axis_step;
				DrawLine3D(draw_origin, draw_end, GREEN);


				draw_origin = cube->vertices[0]->point;
				draw_origin.y = 0;
				draw_origin.z -= axis_step/2;
				draw_end = draw_origin;
				draw_end.z += axis_step*2;
				DrawLine3D(draw_origin, draw_end, GREEN);

				draw_origin.x += axis_step;
				draw_end.x += axis_step;
				DrawLine3D(draw_origin, draw_end, GREEN);
			}
		}
	PROFILE_PAUSE(fill_buffers,3);
	}
}

#define MAX_METAOBJECT_COUNT 32

void DrawMetaSpheres(MetaSphere *metaspheres, int count, float axis_step)
{
	MemoryZeroArray(timers);
	// TODO: grouping by bounding box?
	
	// NOTE: group metaspheres by distance
	int object_group[MAX_METAOBJECT_COUNT] = {0};
	for (int i = 0; i < ArrayCount(object_group); i++)
	{
		object_group[i] = ArrayCount(object_group);
	}
	int group_count = 0;
	BoundingBox group_bounds[MAX_METAOBJECT_COUNT] = {0};
	for (int metasphere_index = 0; metasphere_index < count; metasphere_index++)
	{
		MetaSphere metasphere = metaspheres[metasphere_index];
		Vector3 radius_unit_vector = Vector3Scale(Vector3One(), metasphere.radius);
		int metasphere_group = object_group[metasphere_index];
		// NOTE: not groupped yet
		if (metasphere_group == ArrayCount(object_group)) 
		{
			metasphere_group = group_count++;
			object_group[metasphere_index] = metasphere_group;
			BoundingBox *bounds = group_bounds + metasphere_group;
			bounds->min = Vector3Subtract(metasphere.center, radius_unit_vector);
			bounds->max = Vector3Add(metasphere.center, radius_unit_vector);
		}
		else
		{
			//BoundingBox *bounds = group_bounds + metasphere_group;
			//bounds->min = Vector3Min(bounds->min, Vector3Subtract(metasphere.center, radius_unit_vector));
			//bounds->max = Vector3Max(bounds->max, Vector3Add(metasphere.center, radius_unit_vector));
		}

		for (int other_metasphere_index = metasphere_index + 1; other_metasphere_index < count; other_metasphere_index++)
		{
			MetaSphere other_metasphere = metaspheres[other_metasphere_index];
			if (Vector3Distance(metasphere.center, other_metasphere.center) <= (metasphere.radius + other_metasphere.radius))
			{
				if (object_group[other_metasphere_index] == ArrayCount(object_group))
				{
					object_group[other_metasphere_index] = metasphere_group;
				}
				else
				{
					metasphere_group = object_group[other_metasphere_index];
					object_group[metasphere_index] = metasphere_group;
				}
			}
		}
		BoundingBox *bounds = group_bounds + metasphere_group;
		bounds->min = Vector3Min(bounds->min, Vector3Subtract(metasphere.center, radius_unit_vector));
		bounds->max = Vector3Max(bounds->max, Vector3Add(metasphere.center, radius_unit_vector));
	}

	// NOTE: draw groups
	for (int group_index = 0; group_index < group_count; group_index++)
	{
		MetaSphere group_objects[MAX_METAOBJECT_COUNT] = {0};
		int group_objects_count = 0;
		for (int i = 0; i < count; ++i)
		{
			if (object_group[i] == group_index)
				group_objects[group_objects_count++] = metaspheres[i];
		}

		memory_arena mesh_arena;
		memory_arena normals_arena;
		memory_arena indices_arena;
		memory_arena map_arena;
		// NOTE: Each group shares one memory region due to instant drawing at scope end
		InitializeArena(&mesh_arena,    Megabytes(1), (uint8_t *)global_program_memory.TransientStorage);
		InitializeArena(&normals_arena, Megabytes(1), (uint8_t *)global_program_memory.TransientStorage + Megabytes(1));
		InitializeArena(&indices_arena, Megabytes(1), (uint8_t *)global_program_memory.TransientStorage + Megabytes(2));
		InitializeArena(&map_arena,     Megabytes(1), (uint8_t *)global_program_memory.TransientStorage + Megabytes(3));

		hash_map vertex_map;
		hash_map_init(&vertex_map, &map_arena);

		Mesh mesh = {0};
		mesh.vertices = (float *)mesh_arena.Base;
		mesh.normals =  (float *)normals_arena.Base;
		mesh.indices =  (uint16_t *)indices_arena.Base;
		//MetaSphere metasphere = metaspheres[index];

		//Vector3 radius_unit_vector = Vector3Scale(Vector3One(), metasphere.radius);
		//Vector3 bounding_box_min_corner = Vector3Subtract(metasphere.center, radius_unit_vector);
		Vector3 bounding_box_min_corner = group_bounds[group_index].min;
		// TODO: reduce to bound parallelepiped
		//float bounding_length = metasphere.radius * 2;
		Vector3 bounds_size = Vector3Subtract(group_bounds[group_index].max, bounding_box_min_corner);
		float bounding_length = (float)fmax(bounds_size.x, fmax(bounds_size.y, bounds_size.z));

		int steps_count = (int)(ceil(bounding_length / axis_step));

		Vector3 octree_point = bounding_box_min_corner;
		for (int zn = 0; zn < steps_count; zn++)
		{
			octree_point.y = bounding_box_min_corner.y;
			for (int yn = 0; yn < steps_count; yn++)
			{
				octree_point.x = bounding_box_min_corner.x;
				for (int xn = 0; xn < steps_count; xn++)
				{
					MetaSphereMeshWithOctree(group_objects, group_objects_count, &mesh, &mesh_arena, &normals_arena, &indices_arena, &map_arena, &vertex_map, axis_step/2, octree_point);
					octree_point.x += axis_step/2;
					octree_point.x += axis_step/2;
				}
				octree_point.y += axis_step/2;
				octree_point.y += axis_step/2;
			}
			octree_point.z += axis_step/2;
			octree_point.z += axis_step/2;
		}

		int map_count[BUCKET_COUNT] = {0};
		for (int bucket = 0; bucket < BUCKET_COUNT; bucket++)
		{
			Vector3Node *node = ((Vector3Node **)vertex_map.buckets)[bucket];
			while (node != NULL)
			{
				map_count[bucket]++;
				node = node->next;
			}
		}

		if (IsKeyPressed(KEY_ENTER))
		{
			mesh.texcoords = (float*)map_arena.Base + Megabytes(1);
			MemoryZero(mesh.texcoords, Megabytes(1));
			bool result = ExportMesh(mesh, "mesh.obj");
		}

		PROFILE_START(upload_mesh);
		UploadMesh(&mesh, false);
		PROFILE_END(upload_mesh);

		Model model = LoadModelFromMesh(mesh);
		if (global_shader_valid)
			model.materials[0].shader = shader;


		static Color mb_colors[] = {
			LIGHTGRAY,
			GRAY,      
			DARKGRAY,  
			YELLOW,    
			GOLD,      
			ORANGE,    
			PINK,      
			RED,       
			MAROON,    
			GREEN,     
			LIME,      
			DARKGREEN, 
			SKYBLUE,   
			BLUE,      
			DARKBLUE,  
			PURPLE,    
			VIOLET,    
			DARKPURPLE,
			BEIGE,     
			BROWN,     
			DARKBROWN, 
		};
		static int rnd_index = GetRandomValue(0, ArrayCount(mb_colors));

		//DrawModel(model, {0,0,0}, 1, CLITERAL(Color){ 240, 140, 0, 255 });	
		DrawModel(model, {0,0,0}, 1, mb_colors[rnd_index]);
		if (global_draw_polygon_wires)
			DrawModelWires(model, {0,0,0}, 1, BLACK);

#ifndef MAX_MESH_VERTEX_BUFFERS
#if SUPPORT_GPU_SKINNING
		// NOTE: Two additional vertex buffers required to store bone indices and bone weights
		// WARNING: Some GPUs could not support more than 8 VBOs
		#define MAX_MESH_VERTEX_BUFFERS  9      // Maximum vertex buffers (VBO) per mesh
#else
		#define MAX_MESH_VERTEX_BUFFERS  7      // Maximum vertex buffers (VBO) per mesh
#endif
#endif
		// NOTE: Memory is saved, but performance hits -10 FPS
		if (mesh.vboId != NULL) 
			for (int i = 0; i < MAX_MESH_VERTEX_BUFFERS; i++) 
				rlUnloadVertexBuffer(mesh.vboId[i]);
		RL_FREE(mesh.vboId);
		rlUnloadVertexArray(mesh.vaoId);

		PROFILE_TIMER(compute_force, 0);
		PROFILE_TIMER(collect_hot, 1);
	PROFILE_TIMER(compute_vertex,2);
	PROFILE_TIMER(fill_buffers,3);
	}
}

void GenerateSphere(Vector3 centerPos, float radius, int rings, int slices, Color color)
{
	Mesh sphere_mesh = GenMeshSphere(radius, rings, slices);

	// TODO: understand transform parameter
	//DrawMesh(sphere_mesh, Material material, Matrix transform);
	
	Model sphere_model = LoadModelFromMesh(sphere_mesh);
	DrawModelWires(sphere_model, centerPos, 1, color);
	UnloadModel(sphere_model);
}

Shader MbLoadShader(const char *vsFileName, const char *fsFileName)
{
    Shader new_shader = LoadShader(vsFileName, fsFileName);
	global_shader_valid = IsShaderValid(new_shader);
	if (!global_shader_valid)
	{
		return {0};
	}

    // NOTE(raylib): Get some required shader locations
    new_shader.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(new_shader, "viewPos");
    // NOTE(raylib): "matModel" location name is automatically assigned on shader loading,
    // no need to get the location again if using that uniform name
    //shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(shader, "matModel");

    // Ambient light level (some basic lighting)
    int ambientLoc = GetShaderLocation(new_shader, "ambient");
	float ambient_component = 5;
	float ambient_value[4] = { ambient_component, ambient_component, ambient_component, 1.0f };
    SetShaderValue(new_shader, ambientLoc, ambient_value, SHADER_UNIFORM_VEC4);

    Light light = CreateLight(LIGHT_DIRECTIONAL, {0, 0, 0}, {0, -1, 0}, WHITE, new_shader);
    //Light light1 = CreateLight(LIGHT_DIRECTIONAL, {0, 10, 0}, {0, -1, 0}, BLANK, shader);
    //Light light2 = CreateLight(LIGHT_DIRECTIONAL, {0, 10, 0}, {0, -1, 0}, BLANK, shader);
    //Light light3 = CreateLight(LIGHT_DIRECTIONAL, {0, 10, 0}, {0, -1, 0}, BLANK, shader);
	
	float startCameraPos[3] = { camera.position.x, camera.position.y, camera.position.z };
	SetShaderValue(new_shader, new_shader.locs[SHADER_LOC_VECTOR_VIEW], startCameraPos, SHADER_UNIFORM_VEC3);

	return new_shader;
}

int main()
{
	//
	// Memorandum ======================================
	//
	global_program_memory.PermanentStorageSize = Megabytes(64);
	global_program_memory.TransientStorageSize = Gigabytes(1);

	// TODO(casey): Handle various memory footprints (USING SYSTEM METRICS)
	// TODO(casey): Transient storage needs to be broken up
	// into game transient andt cache transient, and only the
	// former needs to be saved for state playback
	/*
#if HANDMADE_INTERNALO
	LPVOID BaseAddress = (LPVOID)Terabytes(2);
#else
	LPVOID BaseAddress = 0;
#endif
	*/
	// TODO(casey): USE MEM_LARGE_PAGES and call adjust token priveleges
	// when not on Windows XP?
	// NOTE(grigory): TLB pressure should released with large paging. 
	// TODO(grigory): Learn more about TLB in virtual memory unit
	/*
	global_program_memory.PermanentStorage = (void *)VirtualAlloc(
		BaseAddress,
		(size_t)global_program_memory.PermanentStorageSize + global_program_memory.TransientStorageSize,
		MEM_RESERVE|MEM_COMMIT,//|MEM_LARGE_PAGES, 
		PAGE_READWRITE);
	*/
	global_program_memory.PermanentStorage = (void *)MemAlloc(
		(uint32_t)global_program_memory.PermanentStorageSize + (uint32_t)global_program_memory.TransientStorageSize);

	global_program_memory.TransientStorage = (uint8_t *)global_program_memory.PermanentStorage + global_program_memory.PermanentStorageSize;
	//
	// ==================================================
	//
	int screenWidth = 800;
	int screenHeight = 600;
	Vector2 fullscreenRes = {(float)GetScreenWidth(), (float)GetScreenHeight()};
	Vector2 screenRes = {(float)screenWidth, (float)screenHeight};

    SetConfigFlags(FLAG_MSAA_4X_HINT);  // Enable Multi Sampling Anti Aliasing 4x (if available)
	InitWindow(screenWidth, screenHeight, "metaballs");
	SetWindowState(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_MAXIMIZED);


    camera.position = {0.0f, 10.0f, 10.0f};  // Camera position
    camera.target = {0.0f, 0.0f, 0.0f};      // Camera looking at point
    camera.up = {0.0f, 1.0f, 0.0f};          // Camera up vector (rotation towards target)
    camera.fovy = 45.0f;                              // Camera field-of-view Y
    camera.projection = CAMERA_PERSPECTIVE;           // Camera mode type
													  //
	Camera2D camera2d = { 0 };
	camera2d.zoom = 2.0f; // Scales everything by 200%

	rlDisableBackfaceCulling();
#define VERTEX_SHADER   "shaders/glsl330/lighting.vs"
#define FRAGMENT_SHADER "shaders/glsl330/lighting.fs"
	shader = MbLoadShader(VERTEX_SHADER, FRAGMENT_SHADER);
	global_shader_vs_mod_time = GetFileModTime(VERTEX_SHADER);
	global_shader_fs_mod_time = GetFileModTime(FRAGMENT_SHADER);

    //DisableCursor();                    // Limit cursor to relative movement inside the window
    SetTargetFPS(60);

	//Model sphere_model = LoadModel("sphere.obj");
	//Assert(IsModelValid(sphere_model));
	
	MetaSphere metaspheres[MAX_METAOBJECT_COUNT] = {{{0,2,0}, 1}, {{2,2,0}, 1}};
	//MetaSphere* selected_objects[ArrayCount(metaspheres)] = {0};
	int metasphere_count = 2;

    while (!WindowShouldClose())
    {
		if (global_shader_vs_mod_time != GetFileModTime(VERTEX_SHADER) ||
			global_shader_fs_mod_time != GetFileModTime(FRAGMENT_SHADER))
		{
			shader = MbLoadShader(VERTEX_SHADER, FRAGMENT_SHADER);
			global_shader_vs_mod_time = GetFileModTime(VERTEX_SHADER);
			global_shader_fs_mod_time = GetFileModTime(FRAGMENT_SHADER);
		}

		if (IsKeyPressed(KEY_F))
			ToggleFullscreen();

		if (IsKeyPressed(KEY_ZERO))
			global_draw_surfaces_points_count = 0;
		if (IsKeyPressed(KEY_THREE))
			global_draw_surfaces_points_count = 3;
		if (IsKeyPressed(KEY_FOUR))
			global_draw_surfaces_points_count = 4;
		if (IsKeyPressed(KEY_FIVE))
			global_draw_surfaces_points_count = 5;
		if (IsKeyPressed(KEY_SIX))
			global_draw_surfaces_points_count = 6;
		if (IsKeyPressed(KEY_O))
			global_draw_octree_wires = !global_draw_octree_wires;
		if (IsKeyPressed(KEY_G))
			global_draw_octree_grid = !global_draw_octree_grid;
		if (IsKeyPressed(KEY_P))
			global_draw_polygon_wires = !global_draw_polygon_wires;
		// TODO mouse wheel size
		if (IsKeyPressed(KEY_B))
			global_draw_backing_sphere = !global_draw_backing_sphere;
		if (IsKeyPressed(KEY_X))
			global_draw_coordinate_axis = !global_draw_coordinate_axis;

		if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && metasphere_count < ArrayCount(metaspheres))
		{
			metaspheres[metasphere_count] = {Vector3Zero(), 1};
			metasphere_count++;
		}

		//from below DrawRectangle(0, 0, 340, 200, { 232, 232, 232, 255 });
		if ((IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
			 GetMouseWheelMove() != 0.f) &&
			!CheckCollisionRecs({0, 0, 340, 200}, {GetMousePosition().x, GetMousePosition().y, 0, 0}))
		{
			UpdateCamera(&camera, CAMERA_THIRD_PERSON);
			//UpdateCamera(&camera, CAMERA_FREE);
			// Update the shader with the camera view vector (points towards { 0.0f, 0.0f, 0.0f })
			float cameraPos[3] = { camera.position.x, camera.position.y, camera.position.z };
			if (shader.locs != NULL)
				SetShaderValue(shader, shader.locs[SHADER_LOC_VECTOR_VIEW], cameraPos, SHADER_UNIFORM_VEC3);
		}

		//
		// ================ Sphere picking ===========================
		//
		Ray mouse_ray = GetScreenToWorldRay(GetMousePosition(), camera);
        RayCollision collision = { 0 };
        //const char *hitObjectName = "None";
        collision.distance = FLT_MAX;
        collision.hit = false;
        MetaSphere *collided = NULL;
		for (int metasphere_index = 0; metasphere_index < metasphere_count; metasphere_index++)
		{
			MetaSphere metasphere = metaspheres[metasphere_index];
			RayCollision sphereHitInfo = GetRayCollisionSphere(mouse_ray, metasphere.center, metasphere.radius);
			if ((sphereHitInfo.hit) && (sphereHitInfo.distance < collision.distance))
			{
				collision = sphereHitInfo;
				//cursorColor = ORANGE;
				//hitObjectName = "Sphere";
				collided = metaspheres + metasphere_index;	
			}
		}

		if (collision.hit)
		{
			if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
			{
				collided->flags |= MB_OBJECT_SELECTED;
			}
		}
		else
		{
			if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
			{
				for (int metasphere_index = 0; metasphere_index < metasphere_count; metasphere_index++)
				{
					MetaSphere *metasphere = &metaspheres[metasphere_index];
					metasphere->flags &= ~MB_OBJECT_SELECTED;
				}
			}
		}


        BeginDrawing();
		{
			ClearBackground(CLITERAL(Color){ 31, 31, 31, 255 });

			static float grid_step = .5f;
            BeginMode3D(camera);
			{
                //DrawCube(cubePosition, 2.0f, 2.0f, 2.0f, RED);
                //DrawCubeWires(cubePosition, 2.0f, 2.0f, 2.0f, MAROON);
				
				//DrawSphere({0, 0, 0}, 1, YELLOW);
				//DrawSphereEx({0, 0, 0}, 1, 7, 7, YELLOW);
				
				//DrawSphereWires({0, 0, 0}, 1, 15/2, 32/2, BLACK);
				//
				//DrawModelWires(sphere_model, {0,0,0}, 1, BLACK);
				//GenerateSphere({-2,0,0}, 1, 15, 32, BLACK);

				if (global_draw_coordinate_axis)
				{
					DrawRay({{0,0,0},{1,0,0}}, RED);                                                                // Draw a ray line
					DrawRay({{0,0,0},{0,1,0}}, GREEN);                                                                // Draw a ray line
					DrawRay({{0,0,0},{0,0,1}}, BLUE);                                                                // Draw a ray line
				}

				static float speed = 0.02;
				if (IsKeyDown(KEY_LEFT_SHIFT))
					speed = 0.1;
				if (IsKeyReleased(KEY_LEFT_SHIFT))
					speed = 0.02;

				if (IsKeyDown(KEY_LEFT))
					metaspheres[1].center.x -= speed;
				if (IsKeyDown(KEY_RIGHT))
					metaspheres[1].center.x += speed;

				// TODO: move code up
				for (int metasphere_index = 0; metasphere_index < metasphere_count; metasphere_index++)
				{
					MetaSphere *metasphere = &metaspheres[metasphere_index];
					if (metasphere->flags & MB_OBJECT_SELECTED)
					{
						if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
						{
							Vector3 camera_direction = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
							Vector3 plane_center = Vector3Project(metasphere->center, camera_direction);
							Matrix rot = GetCameraMatrix(camera);

							float coef = Vector3Distance(metasphere->center, camera.position) / 1500;
							Vector3 x = Vector3Scale({rot.m0, rot.m4, rot.m8}, GetMouseDelta().x);
							Vector3 y = Vector3Scale({rot.m1, rot.m5, rot.m9}, -GetMouseDelta().y);
							Vector3 delta = Vector3Add(Vector3Scale(x, coef), Vector3Scale(y, coef));

							metasphere->center = Vector3Add(metasphere->center, delta);
						}
					}
				}

				if (IsKeyDown(KEY_UP))
					THRESHOLD -= speed;
				if (IsKeyDown(KEY_DOWN))
					THRESHOLD += speed;
				//metaspheres[0].radius = 1 - THRESHOLD;
				//metaspheres[1].radius = 1 - THRESHOLD;
				metaspheres[0].radius = 1;
				metaspheres[1].radius = 1;

				if (IsKeyPressed(KEY_MINUS))
					grid_step -= 0.1;
				if (IsKeyPressed(KEY_EQUAL))
					grid_step += 0.1;
				
				PROFILE_START(draw);
				DrawMetaSpheres(metaspheres, metasphere_count, grid_step);
				PROFILE_END(draw);
				//DrawMetaSphere(metaspheres, 1, grid_step, Vector3Add({-1,-1,-1}, metaspheres[1].center));

				//if (global_draw_backing_sphere)
				//	DrawSphere({0, 0, 0}, 0.9, RED);
				
				if (collision.hit)
                {
                    //DrawCube(collision.point, 0.3f, 0.3f, 0.3f, GREEN);
                    DrawCubeWires(collision.point, 0.3f, 0.3f, 0.3f, GREEN);
					/*
                    Vector3 normalEnd;
                    normalEnd.x = collision.point.x + collision.normal.x;
                    normalEnd.y = collision.point.y + collision.normal.y;
                    normalEnd.z = collision.point.z + collision.normal.z;

                    DrawLine3D(collision.point, normalEnd, RED);
					*/
                }
			}
            EndMode3D();

			for (int metasphere_index = 0; metasphere_index < metasphere_count; metasphere_index++)
			{
				MetaSphere *metasphere = &metaspheres[metasphere_index];
				if (metasphere->flags & MB_OBJECT_SELECTED)
				{
					Vector2 selected_center = GetWorldToScreen(metasphere->center, camera);
					// TODO: draw proper 3d circle, there is sphere distortion in corners
					float selected_radius = 1 / Vector3Distance(metasphere->center, camera.position) * 1500;
					DrawCircleLinesV(selected_center, selected_radius, GREEN);
				}
			}

			float scale = 1.2; // Scale up by 150%

			// Scale fonts and spacing globally
			GuiSetStyle(DEFAULT, TEXT_SIZE, (int)(16 * scale));
			GuiSetStyle(DEFAULT, TEXT_SPACING, (int)(1 * scale));

			// Draw GUI controls
			//------------------------------------------------------------------------------
			int width = GetScreenWidth();
			int height = GetScreenHeight();
			DrawFPS(width - 80, 0);                                                     // Draw current FPS

			DrawRectangle(0, 0, 370, 275, { 232, 232, 232, 255 });

			float margin_x = 110;
			GuiSliderBar({ margin_x, 30, 120, 20}, "Grid size", TextFormat("%.2f", grid_step), &grid_step, .2f, 2);
			GuiSliderBar({ margin_x, 60, 120, 20 }, "Threshold", TextFormat("%.2f", THRESHOLD), &THRESHOLD, .0f, 1);
			GuiCheckBox({ margin_x, 90, 20, 20 }, "Show volume grid", &global_draw_octree_wires);
			GuiCheckBox({ margin_x, 120, 20, 20 }, "Show projected grid", &global_draw_octree_grid);
			GuiCheckBox({ margin_x, 150, 20, 20 }, "Show polygons", &global_draw_polygon_wires);
			GuiCheckBox({ margin_x, 180, 20, 20 }, "Show XY axis", &global_draw_coordinate_axis);

			DrawText("LMB to rotate scene\nRMB to move sphere", 20, 210, 20, GRAY);
			//------------------------------------------------------------------------------
			
		}
        EndDrawing();
    }

    CloseWindow();

	//while (true) {}
    return 0;
}

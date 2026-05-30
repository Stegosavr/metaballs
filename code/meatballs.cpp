#define _CRT_SECURE_NO_WARNINGS 1
#include "../include/raylib.h"
#include "../include/raymath.h"
#include "../include/rlgl.h"
#define RAYGUI_IMPLEMENTATION
#include "../include/raygui.h"

#include "stdlib.h"
#include "stdint.h"
#include "math.h"

#define internal static

#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))
#define Assert(Expression) if (!(Expression)) *(int *)0 = 0;
#define memory_index size_t

#define Kilobytes(Value) ((Value)*1024ULL)
#define Megabytes(Value) (Kilobytes(Value)*1024ULL)
#define Gigabytes(Value) (Megabytes(Value)*1024ULL)
#define Terabytes(Value) (Gigabytes(Value)*1024ULL)

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

struct Size
{
	int Width;
	int Height;
};

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
};

static float THRESHOLD = 0.2f;
float MetaSphereForceInPoint(Vector3 point, MetaSphere *metaspheres, int count)
{
	//NOTE: Positive inside sphere, negative outside
	
	//float force = Square(metasphere.radius) - Vector3DistanceSqr(point, metasphere.center);
	//float force = metasphere.radius - Vector3Distance(point, metasphere.center);
	
	//for (int i = 0; i < count; i++)
	float force = metaspheres[0].radius - Vector3Distance(point, metaspheres[0].center);
	float force2 = metaspheres[1].radius - Vector3Distance(point, metaspheres[1].center);

	float result = 0;
	if (force > force2)
	{
		result = force;
		if (force2 > 0)
			result += force2;
	}
	else
	{
		result = force2;
		if (force > 0)
			result += force;
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

#define OCTREE_RESOLUTION 1
#define OCTREE_CUBES 8
#define OCTREE_AXIS_POINTS 3
void MetaSphereMeshWithOctree(MetaSphere *metaspheres, Mesh *mesh, memory_arena *arena, int index, float axis_step, Vector3 start_point)
{
	MetaSphere metasphere = metaspheres[index];

	OctreeVertex* octree_vertices = (OctreeVertex *)malloc(sizeof(OctreeVertex)*3*3*3);
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
				float force = MetaSphereForceInPoint(octree_point, metaspheres, 2);
				octree_vertices[((zn)*OCTREE_AXIS_POINTS + yn)*OCTREE_AXIS_POINTS+xn] = {octree_point, force, force > THRESHOLD};

				//if (force > 0)
				//	refine = true;

				octree_point.x += axis_step;
			}
			octree_point.y += axis_step;
		}
		octree_point.z += axis_step;
	}

	int cube_origins[8] = {0, 1, 3, 4, 9, 10, 12, 13};
	int vertex_edge_offsets[6] = {1, 2, 4, -1, -2, -4};
	for (int i = 0; i < 8; i++)
	{
		OctreeCube *cube = &cubes[i];
		int origin = cube_origins[i];
		
		int hot_count = 0;
		//
		// TODO: use this bitfield to bake poligons to table
		//
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
		
		//Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
		//DrawCubeWires(draw_origin, axis_step, axis_step, axis_step, MAROON);

		// NOTE: should go away when prev todo completed
		int vertex_surface[8] = {0};
		int current_surface = 1;
		// 4 sufaces 16 points each
		Vector3 surface_points[4*16] = {0};
		int surface_points_count[4] = {0};

		for (int j = 0; j < 8; j++)
		{
			OctreeVertex *vertex = cube->vertices[j];
			int adjacent_surface = 0;
			for (int offs = 0; offs < 6; offs++)
			{
				// TODO: this offset table works only for some points, hack for now
				int adjacent_index = j + vertex_edge_offsets[offs];
				if (adjacent_index < 0 || adjacent_index > 7)
				{
					continue;
				}

				OctreeVertex *adjacent_vertex = cube->vertices[adjacent_index];
				int coord_match = ((vertex->point.x == adjacent_vertex->point.x) + 
								   (vertex->point.y == adjacent_vertex->point.y) + 
								   (vertex->point.z == adjacent_vertex->point.z));
				if (coord_match != 2)
				{
					continue;
				}
				// NOTE: should go away when prev todo completed
				if (vertex->hot)
				{
					if (vertex_surface[adjacent_index] != 0) 
					{
						vertex_surface[j] = vertex_surface[adjacent_index];
					}
					else if (adjacent_vertex->hot)
					{
						vertex_surface[adjacent_index] = current_surface;
					}
				}

				if ((adjacent_vertex->hot != vertex->hot) &&
					vertex->hot)
				{
					// TODO: force is squared function, so linear interpolation is wrong i guess
					//
					//t = (THRESHOLD - Force at B) / (Force at A - Force at B)
					//P = B*(1-t) + A*t;
					float t = (THRESHOLD - adjacent_vertex->force) / (vertex->force - adjacent_vertex->force);
					Vector3 estimated_point = Vector3Add(Vector3Scale(adjacent_vertex->point, 1-t), Vector3Scale(vertex->point, t));

					{
						if (vertex->hot && (vertex_surface[j] == 0))
						{
							vertex_surface[j] = current_surface;
							//current_surface++;
						}
						int surface_index = vertex_surface[j] - 1;
						surface_points[surface_points_count[surface_index]] = estimated_point;
						if (Vector3Equals(estimated_point , {-0.25, -0.75, -0.75}) && surface_points_count[surface_index] == 3)
						{
							malloc(0);
						}
						surface_points_count[surface_index]++;
					}
				}
			}
		}

		// TODO: process more than 1 surface (arena push and surface computing in code above)
		// TODO: explain or do better this random arena push
		//mesh.vertices = (float *)malloc(3*sizeof(float)*72);    
		int vertex_count = (surface_points_count[0] - 2) * 3;
		if (surface_points_count[0] == 5 || surface_points_count[0] == 6)
			vertex_count *= 2;
		Vector3 *mesh_vertex = (Vector3*)PushArray(arena, vertex_count, Vector3);
		for (int surface_index = 0; surface_index < 4; surface_index++)
		{
			int points = surface_points_count[surface_index];
			Vector3 *surface = &surface_points[surface_index*16];
			if (points == 3 &&
				(global_draw_surfaces_points_count == 3 || global_draw_surfaces_points_count == 0))
			{
				if (global_draw_octree_wires)
				{
					Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
					DrawCubeWires(draw_origin, axis_step, axis_step, axis_step, GREEN);
				}
				if (global_draw_octree_grid)
				{
					Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
					draw_origin.y = 0;
					DrawCubeWires(draw_origin, axis_step, 0, axis_step, GREEN);

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

				mesh->triangleCount += 1;
				mesh->vertexCount += 3;

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[2];
			}
			if (points == 4 &&
				(global_draw_surfaces_points_count == 4 || global_draw_surfaces_points_count == 0))
			{
				if (global_draw_octree_wires)
				{
					Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
					DrawCubeWires(draw_origin, axis_step, axis_step, axis_step, GREEN);
				}
				if (global_draw_octree_grid)
				{
					Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
					draw_origin.y = 0;
					DrawCubeWires(draw_origin, axis_step, 0, axis_step, GREEN);

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

				mesh->triangleCount += 2;
				mesh->vertexCount += 6;

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[2];

				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[2];
				*mesh_vertex++ = surface[3];
			}
			if (points == 5 &&
				(global_draw_surfaces_points_count == 5 || global_draw_surfaces_points_count == 0))
			{
				if (global_draw_octree_wires)
				{
					Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
					DrawCubeWires(draw_origin, axis_step, axis_step, axis_step, GREEN);
				}

				mesh->triangleCount += 3;
				mesh->vertexCount += 9;

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[2];

				*mesh_vertex++ = surface[2];
				*mesh_vertex++ = surface[3];
				*mesh_vertex++ = surface[4];

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[2];
				*mesh_vertex++ = surface[4];

				//shish
				mesh->triangleCount += 3;
				mesh->vertexCount += 9;

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[3];

				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[2];
				*mesh_vertex++ = surface[3];

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[4];
			}
			if (points == 6 &&
				(global_draw_surfaces_points_count == 6 || global_draw_surfaces_points_count == 0))
			{
				if (global_draw_octree_wires)
				{
					Vector3 draw_origin = Vector3Add(cube->vertices[0]->point, Vector3Scale(Vector3One(), axis_step/2));
					DrawCubeWires(draw_origin, axis_step, axis_step, axis_step, GREEN);
				}

				mesh->triangleCount += 4;
				mesh->vertexCount += 12;

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[2];

				*mesh_vertex++ = surface[2];
				*mesh_vertex++ = surface[3];
				*mesh_vertex++ = surface[4];

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[2];
				*mesh_vertex++ = surface[4];

				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[3];
				*mesh_vertex++ = surface[5];

				mesh->triangleCount += 4;
				mesh->vertexCount += 12;

				*mesh_vertex++ = surface[4];
				*mesh_vertex++ = surface[5];
				*mesh_vertex++ = surface[1];

				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[2];
				*mesh_vertex++ = surface[5];

				*mesh_vertex++ = surface[3];
				*mesh_vertex++ = surface[4];
				*mesh_vertex++ = surface[5];

				*mesh_vertex++ = surface[0];
				*mesh_vertex++ = surface[1];
				*mesh_vertex++ = surface[5];
			}
			if ((points < 3 || points > 5) && points != 0)
			{
				continue;
			}
		}
		//UploadMesh(&mesh, false);

		/* //test light shit
		mesh.normals = (float *)malloc(3*sizeof(float)*72);    
		Vector3 *mesh_normal = (Vector3*)mesh.normals;
		mesh_vertex = (Vector3*)mesh.vertices;
		for (int k = 0; k < mesh.vertexCount; ++k)
		{
			*mesh_normal++ = Vector3Normalize(Vector3Subtract(mesh_vertex[k], metasphere.center));
		}
		*/

		/*
		Model model = LoadModelFromMesh(mesh);
		//model.materials[0].shader = shader;
		DrawModel(model, {0,0,0}, 1, CLITERAL(Color){ 240, 140, 0, 255 });	
		if (global_draw_polygon_wires)
			DrawModelWires(model, {0,0,0}, 1, BLACK);
		UnloadModel(model);
		*/
	}

	free(octree_vertices);
}

void DrawMetaSphere(MetaSphere *metaspheres, int index, float axis_step, Vector3 start_point)
{
	memory_arena mesh_arena;
	InitializeArena(&mesh_arena, Megabytes(1), (uint8_t *)global_program_memory.TransientStorage);
	Mesh mesh = {0};
	mesh.vertices = (float *)mesh_arena.Base;
	MetaSphere metasphere = metaspheres[index];

	Vector3 radius_unit_vector = Vector3Scale(Vector3One(), metasphere.radius);
	Vector3 bounding_box_min_corner = Vector3Subtract(metasphere.center, radius_unit_vector);

	float bounding_length = metasphere.radius * 2;
	int steps_count = (int)(ceil(bounding_length / axis_step));

	Vector3 octree_point = start_point;
	for (int zn = 0; zn < steps_count; zn++)
	{
		octree_point.y = start_point.y;
		for (int yn = 0; yn < steps_count; yn++)
		{
			octree_point.x = start_point.x;
			for (int xn = 0; xn < steps_count; xn++)
			{
				MetaSphereMeshWithOctree(metaspheres, &mesh, &mesh_arena, index, axis_step/2, octree_point);
				octree_point.x += axis_step;
			}
			octree_point.y += axis_step;
		}
		octree_point.z += axis_step;
	}

	UploadMesh(&mesh, false);
	
	Model model = LoadModelFromMesh(mesh);
	DrawModel(model, {0,0,0}, 1, CLITERAL(Color){ 240, 140, 0, 255 });	
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

	rlDisableBackfaceCulling();

	Camera3D camera = {0};
    camera.position = {0.0f, 10.0f, 10.0f};  // Camera position
    camera.target = {0.0f, 0.0f, 0.0f};      // Camera looking at point
    camera.up = {0.0f, 1.0f, 0.0f};          // Camera up vector (rotation towards target)
    camera.fovy = 45.0f;                              // Camera field-of-view Y
    camera.projection = CAMERA_PERSPECTIVE;           // Camera mode type
	
	Camera2D camera2d = { 0 };
	camera2d.zoom = 2.0f; // Scales everything by 200%

    //DisableCursor();                    // Limit cursor to relative movement inside the window
    SetTargetFPS(60);

	//Model sphere_model = LoadModel("sphere.obj");
	//Assert(IsModelValid(sphere_model));

    while (!WindowShouldClose())
    {
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

		//from below DrawRectangle(0, 0, 340, 200, { 232, 232, 232, 255 });
		if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
			!CheckCollisionRecs({0, 0, 340, 200}, {GetMousePosition().x, GetMousePosition().y, 0, 0}))
		{
			UpdateCamera(&camera, CAMERA_THIRD_PERSON);
		}


        BeginDrawing();
		{
			ClearBackground(CLITERAL(Color){ 31, 31, 31, 255 });

			static float grid_step = 0.5f;
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
				}

				static float speed = 0.02;
				static MetaSphere metaspheres[2] = {{{0,2,0}, 1}, {{2,2,0}, 1}};
				if (IsKeyDown(KEY_LEFT_SHIFT))
					speed = 0.1;
				if (IsKeyReleased(KEY_LEFT_SHIFT))
					speed = 0.02;

				if (IsKeyDown(KEY_LEFT))
					metaspheres[1].center.x -= speed;
				if (IsKeyDown(KEY_RIGHT))
					metaspheres[1].center.x += speed;

				if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
				{
					metaspheres[1].center.x += GetMouseDelta().x / 100.f;
					metaspheres[1].center.y += -GetMouseDelta().y / 100.f;
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
				
				DrawMetaSphere(metaspheres, 0, grid_step, Vector3Add({-1,-1,-1}, metaspheres[0].center));
				DrawMetaSphere(metaspheres, 1, grid_step, Vector3Add({-1,-1,-1}, metaspheres[1].center));

				//if (global_draw_backing_sphere)
				//	DrawSphere({0, 0, 0}, 0.9, RED);
			}
            EndMode3D();

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
			GuiSliderBar({ margin_x, 30, 120, 20}, "Grid size", TextFormat("%.0f", grid_step), &grid_step, .2f, 2);
			GuiSliderBar({ margin_x, 60, 120, 20 }, "Threshold", TextFormat("%.0f", THRESHOLD), &THRESHOLD, .0f, 1);
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

    return 0;
}

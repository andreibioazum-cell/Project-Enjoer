/* Public C scripting API for Enjoer engine projects.
 *
 * A project script is a regular C file compiled to a shared object; the engine
 * loads it with dlopen and calls the two optional entry points:
 *
 *   void eng_script_ready(EngNode *self);     once, after the scene exists
 *   void eng_script_process(EngNode *self, float dt);   every frame
 *
 * The script links against the functions below (the host binary exports them;
 * build the .so with `cc -shared -fPIC script.c -o script.so` and -I pointing
 * at src/engine). */
#ifndef ENG_API_H
#define ENG_API_H

typedef struct EngNode EngNode;

/* ── logging ────────────────────────────────────────────────────────────── */
void eng_print(const char *message);

/* ── node tree ──────────────────────────────────────────────────────────── */
/* Create a node of the given type ("Node3D", "MeshInstance3D", "Camera3D",
 * "DirectionalLight3D", "OmniLight3D", "VoxelWorld3D"). Not attached yet. */
EngNode *eng_node_new(const char *type, const char *name);
void eng_node_add_child(EngNode *parent, EngNode *child);
void eng_node_free(EngNode *node);              /* detaches and frees subtree */
EngNode *eng_node_find(const char *path);       /* "Root/Cube", from the root */
const char *eng_node_name(const EngNode *node);
const char *eng_node_type(const EngNode *node);

/* ── Node3D transform ───────────────────────────────────────────────────── */
void eng_node3d_set_position(EngNode *node, float x, float y, float z);
void eng_node3d_get_position(EngNode *node, float *x, float *y, float *z);
/* Euler radians: yaw (Y), pitch (X), roll (Z), applied Ry*Rx*Rz. */
void eng_node3d_set_rotation(EngNode *node, float yaw, float pitch, float roll);
void eng_node3d_get_rotation(EngNode *node, float *yaw, float *pitch, float *roll);
void eng_node3d_set_scale(EngNode *node, float scale);

/* ── Camera3D ───────────────────────────────────────────────────────────── */
void eng_camera_make_current(EngNode *camera);

/* ── lights ─────────────────────────────────────────────────────────────── */
void eng_light_set_energy(EngNode *light, float energy);
void eng_light_set_color(EngNode *light, float r, float g, float b);
void eng_light_set_range(EngNode *omni, float range); /* OmniLight3D only */

/* ── MeshInstance3D ─────────────────────────────────────────────────────── */
/* mesh: "cube", "plane", "sphere", "cylinder" */
void eng_mesh_set(EngNode *node, const char *mesh);
void eng_mesh_set_color(EngNode *node, float r, float g, float b);
void eng_mesh_set_size(EngNode *node, float size);

/* ── misc ───────────────────────────────────────────────────────────────── */
double eng_time(void);        /* seconds since the engine started */
float eng_delta(void);        /* last frame interval */

#endif

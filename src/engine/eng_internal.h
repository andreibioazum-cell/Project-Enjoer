/* Internal engine API: node tree, scene files, projects, scripting, rendering. */
#ifndef ENG_INTERNAL_H
#define ENG_INTERNAL_H
#include "engine.h"
#include "eng_api.h"
#include "geometrium/geometrium_render_internal.h"

enum {
    ENG_NODE, ENG_NODE3D, ENG_MESH, ENG_CAMERA,
    ENG_DIR_LIGHT, ENG_OMNI_LIGHT, ENG_VOXEL_WORLD
};

typedef struct { float m[9]; } EngBasis;   /* row-major 3x3, v' = M*v */
typedef struct { float x, y, z; } EngVec;
/* EngNode is forward-declared in eng_api.h. */

struct EngNode {
    char name[32];
    int type;
    struct EngNode *parent, *child, *next; /* child list + sibling link */
    EngVec pos; float yaw, pitch, roll, scale;
    /* cached global transform */
    EngBasis basis; EngVec gpos; float gscale; int dirty;
    /* MeshInstance3D */
    int mesh_kind;                 /* ENG_MESH_KIND_* */
    float size;
    uint32_t color; int has_color;
    int block;                     /* voxel texture material, 0 = none */
    /* Camera3D */
    float fov; int current;
    /* lights */
    float energy, range; float lcol[3];
    /* scripting */
    struct EngScript *script;
};
enum { ENG_MESH_NONE, ENG_MESH_CUBE, ENG_MESH_PLANE, ENG_MESH_SPHERE, ENG_MESH_CYLINDER };

typedef struct EngScript EngScript;
struct EngScript {
    char name[32];
    void *handle;
    void (*ready)(EngNode *);
    void (*process)(EngNode *, float);
    EngScript *next;
};

/* ── node.c ── */
EngNode *eng_node_create(int type, const char *name);
void eng_node_attach(EngNode *parent, EngNode *child);
void eng_node_destroy(EngNode *node);
EngNode *eng_node_root(void);
void eng_node_set_root(EngNode *root);
void eng_node_update_transforms(void);
const EngBasis *eng_node_basis(const EngNode *node);
int eng_type_from_name(const char *type);
const char *eng_type_name(int type);
void eng_basis_euler(EngBasis *out, float yaw, float pitch, float roll);

/* ── mesh.c ── */
typedef struct { int count; EngVec v[8]; EngVec n; float uv[8][2]; } EngFace;
int eng_mesh_faces(int kind, const EngFace **out); /* built-in primitives */
GeometriumMaterial *eng_flat_material(uint32_t argb);

/* ── scene.c ── */
void eng_scene_set_project_dir(const char *dir);
int eng_scene_load(const char *path);              /* .escn text scene */
void eng_scene_free(void);
void eng_update(float dt);                          /* scripts + voxel world */
int eng_draw(Buffer *buffer);                       /* camera, lights, meshes */

/* ── script.c ── */
EngScript *eng_script_load(const char *name, const char *project_dir);
void eng_script_ready_run(void);
void eng_script_process_all(float dt);
void eng_script_discard_all(void);

/* ── project.c ── */
int eng_project_load(const char *directory);
void eng_project_free(void);
int eng_project_active(void);
const char *eng_project_name(void);

/* ── api.c ── */
int eng_init(AAssetManager *assets);
void eng_time_internal(double now, double dt);

#endif

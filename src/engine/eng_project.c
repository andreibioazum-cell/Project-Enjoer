/* Project loader: project.eng manifest (name, main_scene) + scenes + scripts. */
#include "eng_internal.h"
#include <stdio.h>
#include <string.h>

static char project_name[64];
static char project_path[4096];
static int active;

int eng_project_load(const char *directory) {
    char manifest[4096];
    snprintf(manifest, sizeof(manifest), "%s/project.eng", directory);
    FILE *f = fopen(manifest, "r");
    if (!f) {
        fprintf(stderr, "engine: no project.eng in '%s'\n", directory);
        return 0;
    }
    char main_scene[256] = "";
    project_name[0] = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *key = line;
        char *end = eq;
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = 0;
        while (*key == ' ' || *key == '\t') key++;
        char *value = eq + 1;
        while (*value == ' ' || *value == '\t') value++;
        end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) end--;
        *end = 0;
        if (!strcmp(key, "name")) snprintf(project_name, sizeof(project_name), "%s", value);
        else if (!strcmp(key, "main_scene")) snprintf(main_scene, sizeof(main_scene), "%s", value);
    }
    fclose(f);
    if (!main_scene[0]) {
        fprintf(stderr, "engine: project '%s' has no main_scene\n", directory);
        return 0;
    }
    char scene_path[4096];
    if (main_scene[0] == '/') snprintf(scene_path, sizeof(scene_path), "%s", main_scene);
    else snprintf(scene_path, sizeof(scene_path), "%s/%s", directory, main_scene);
    eng_scene_set_project_dir(directory);
    if (!eng_scene_load(scene_path)) return 0;
    snprintf(project_path, sizeof(project_path), "%s", directory);
    active = 1;
    return 1;
}
void eng_project_free(void) {
    eng_scene_free();
    eng_script_discard_all();
    active = 0;
}
int eng_project_active(void) { return active; }
const char *eng_project_name(void) { return project_name; }

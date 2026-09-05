/* core/strings.c — пул отслеживаемых строк и строковые хелперы.
 * Все строки, возвращаемые рантаймом, живут в пуле и освобождаются
 * разом через ds_string_pool_reset(). */
#include "runtime.h"
#include <stdio.h>

typedef struct DSStringNode DSStringNode;
struct DSStringNode { DSStringNode *next; char *string; };

static DSStringNode *ds_strings = NULL;

char *ds_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s)+1;
    char *c = (char*)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

char *ds_track_string(char *s) {
    if (!s) { ds_runtime_error("out of memory string"); return NULL; }
    DSStringNode *node = (DSStringNode*)malloc(sizeof(*node));
    if (!node) { free(s); ds_runtime_error("out of memory tracking"); return NULL; }
    node->string = s; node->next = ds_strings; ds_strings = node;
    return s;
}

char *ds_num_to_string(double number) {
    char buf[96];
    if (snprintf(buf, sizeof(buf), "%g", number) < 0) return NULL;
    return ds_track_string(ds_strdup(buf));
}

void ds_string_pool_reset(void) {
    DSStringNode *node = ds_strings;
    while (node) { DSStringNode *next = node->next; free(node->string); free(node); node = next; }
    ds_strings = NULL;
}

char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0, lb = right ? strlen(right) : 0;
    char *out = (char*)malloc(la+lb+1);
    if (!out) return ds_track_string(ds_strdup(""));
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out+la, right, lb);
    out[la+lb] = '\0';
    return ds_track_string(out);
}

double clamp(double v, double lo, double hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
double lerp(double a, double b, double t){ return a + (b-a)*t; }
double dist(double x1, double y1, double x2, double y2){ double dx=x2-x1, dy=y2-y1; return sqrt(dx*dx+dy*dy); }

double str_len(const char *s){ return s ? (double)strlen(s) : 0; }
int str_eq(const char *a, const char *b){ if(a==b) return 1; if(!a||!b) return 0; return strcmp(a,b)==0; }
int str_contains(const char *hay, const char *needle){
    if(!hay||!needle) return 0;
    if(!*needle) return 1;
    return strstr(hay, needle)!=NULL ? 1 : 0;
}
double str_index_of(const char *hay, const char *needle){
    if(!hay||!needle) return -1;
    const char *p = strstr(hay, needle);
    if(!p) return -1;
    return (double)(p - hay);
}
int str_starts_with(const char *s, const char *pref){
    if(!s||!pref) return 0;
    size_t ls=strlen(s), lp=strlen(pref);
    if(lp>ls) return 0;
    return strncmp(s,pref,lp)==0 ? 1 : 0;
}
int str_ends_with(const char *s, const char *suf){
    if(!s||!suf) return 0;
    size_t ls=strlen(s), lf=strlen(suf);
    if(lf>ls) return 0;
    return strcmp(s+ls-lf,suf)==0 ? 1 : 0;
}
const char *str_sub(const char *s, double start, double len){
    if(!s) return ds_track_string(ds_strdup(""));
    size_t sl=strlen(s);
    long st=(long)start;
    long ln=(long)len;
    if(st<0) st=0;
    if((size_t)st>sl) st=sl;
    if(ln<0) ln=0;
    if((size_t)(st+ln)>sl) ln=sl-st;
    char *out=(char*)malloc((size_t)ln+1);
    if(!out) return ds_track_string(ds_strdup(""));
    memcpy(out,s+st,(size_t)ln);
    out[ln]='\0';
    return ds_track_string(out);
}
double str_to_num(const char *s){
    if(!s) return 0;
    char *end=NULL;
    double v=strtod(s,&end);
    if(end==s) return 0;
    return v;
}
const char *str_trim(const char *s){
    if(!s) return ds_track_string(ds_strdup(""));
    const char *a=s;
    while(*a && (*a==' '||*a=='\t'||*a=='\n'||*a=='\r')) a++;
    const char *b=s+strlen(s);
    while(b>a && (b[-1]==' '||b[-1]=='\t'||b[-1]=='\n'||b[-1]=='\r')) b--;
    size_t ln=b-a;
    char *out=(char*)malloc(ln+1);
    if(!out) return ds_track_string(ds_strdup(""));
    memcpy(out,a,ln);
    out[ln]='\0';
    return ds_track_string(out);
}
const char *str_lower(const char *s){
    if(!s) return ds_track_string(ds_strdup(""));
    size_t l=strlen(s);
    char *out=(char*)malloc(l+1);
    if(!out) return ds_track_string(ds_strdup(""));
    for(size_t i=0;i<l;i++){
        char c=s[i];
        if(c>='A'&&c<='Z') c=c-'A'+'a';
        out[i]=c;
    }
    out[l]='\0';
    return ds_track_string(out);
}
const char *str_upper(const char *s){
    if(!s) return ds_track_string(ds_strdup(""));
    size_t l=strlen(s);
    char *out=(char*)malloc(l+1);
    if(!out) return ds_track_string(ds_strdup(""));
    for(size_t i=0;i<l;i++){
        char c=s[i];
        if(c>='a'&&c<='z') c=c-'a'+'A';
        out[i]=c;
    }
    out[l]='\0';
    return ds_track_string(out);
}

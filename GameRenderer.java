package com.enjoer.game;
import android.content.Context;
import android.opengl.GLES30;
import android.opengl.GLSurfaceView;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class GameRenderer implements GLSurfaceView.Renderer {
    private String fs = "#version 300 es\nprecision highp float;" +
        "uniform vec3 camP; out vec4 fC; in vec3 vP;" +
        "float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}" +
        "void main(){" +
        "  vec3 d = normalize(vP - camP);" +
        "  vec3 sky = mix(vec3(0.01,0.02,0.05), vec3(0.0,0.1,0.3), clamp(d.y*0.5+0.5,0.0,1.0));" +
        "  if(d.y > 0.05){" +
        "    vec2 uv = d.xz/(d.y+0.7);" + // Звезды ТЕПЕРЬ ВВЕРХУ
        "    if(h(floor(uv*100.0))>0.98) sky += 0.8;" +
        "  }" +
        "  fC = vec4(sky, 1.0);" +
        "}";

    public GameRenderer(Context ctx) {}
    @Override public void onSurfaceCreated(GL10 gl, EGLConfig c) { GLES30.glEnable(GLES30.GL_DEPTH_TEST); }
    @Override public void onSurfaceChanged(GL10 gl, int w, int h) { GLES30.glViewport(0,0,w,h); }
    @Override public void onDrawFrame(GL10 gl) { GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT | GLES30.GL_DEPTH_BUFFER_BIT); }
}

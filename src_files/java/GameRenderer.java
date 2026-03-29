package com.enjoer.game;
import android.opengl.*;
import java.nio.*;
import java.util.concurrent.CopyOnWriteArrayList;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
public class GameRenderer implements GLSurfaceView.Renderer {
    private final float[] proj=new float[16], view=new float[16], model=new float[16];
    public float pX=0, pY=6, pZ=15, rY=0, rX=0, jX=0, jY=0, vY=0;
    public boolean isEngineMode=false;
    private int prog;
    private FloatBuffer vBuf;
    public CopyOnWriteArrayList<Block> blocks = new CopyOnWriteArrayList<>();
    @Override public void onSurfaceCreated(GL10 gl, EGLConfig cfg) {
        GLES30.glEnable(GLES30.GL_DEPTH_TEST);
        String vs="#version 300 es\nlayout(location=0)in vec4 aP;uniform mat4 uMVP;void main(){gl_Position=uMVP*aP;}";
        String fs="#version 300 es\nprecision highp float;uniform vec4 uC;out vec4 fC;void main(){fC=uC;}";
        prog=GLES30.glCreateProgram();
        int vsh=GLES30.glCreateShader(35633); GLES30.glShaderSource(vsh,vs); GLES30.glCompileShader(vsh);
        int fsh=GLES30.glCreateShader(35632); GLES30.glShaderSource(fsh,fs); GLES30.glCompileShader(fsh);
        GLES30.glAttachShader(prog,vsh); GLES30.glAttachShader(prog,fsh); GLES30.glLinkProgram(prog);
        float[] vc={-.5f,.5f,.5f, -.5f,-.5f,.5f, .5f,.5f,.5f, .5f,-.5f,.5f, -.5f,.5f,-.5f, -.5f,-.5f,-.5f, .5f,.5f,-.5f, .5f,-.5f,-.5f, -.5f,.5f,-.5f, -.5f,.5f,.5f, .5f,.5f,-.5f, .5f,.5f,.5f, -.5f,-.5f,-.5f, -.5f,-.5f,.5f, .5f,-.5f,-.5f, .5f,-.5f,.5f, -.5f,.5f,-.5f, -.5f,-.5f,-.5f, -.5f,.5f,.5f, -.5f,-.5f,.5f, .5f,.5f,-.5f, .5f,-.5f,-.5f, .5f,.5f,.5f, .5f,-.5f,.5f};
        vBuf=ByteBuffer.allocateDirect(vc.length*4).order(ByteOrder.nativeOrder()).asFloatBuffer().put(vc); vBuf.position(0);
        blocks.add(new Block(0,1,-5, 20,2,20, 0.1f,0.1f,0.2f));
    }
    @Override public void onSurfaceChanged(GL10 gl,int w,int h){ GLES30.glViewport(0,0,w,h); Matrix.perspectiveM(proj,0,60,(float)w/h,0.1f,500f); }
    @Override public void onDrawFrame(GL10 gl) {
        update(); GLES30.glClearColor(0,0,0.01f,1); GLES30.glClear(16640);
        float vx=(float)(Math.sin(Math.toRadians(rY))*Math.cos(Math.toRadians(rX))), vy=(float)Math.sin(Math.toRadians(rX)), vz=(float)(-Math.cos(Math.toRadians(rY))*Math.cos(Math.toRadians(rX)));
        Matrix.setLookAtM(view,0,pX,pY,pZ,pX+vx,pY+vy,pZ+vz,0,1,0);
        GLES30.glUseProgram(prog); int uMVP=GLES30.glGetUniformLocation(prog,"uMVP"), uC=GLES30.glGetUniformLocation(prog,"uC");
        GLES30.glVertexAttribPointer(0,3,GLES30.GL_FLOAT,false,0,vBuf); GLES30.glEnableVertexAttribArray(0);
        for(Block b : blocks){
            Matrix.setIdentityM(model,0); Matrix.translateM(model,0,b.x,b.y,b.z); Matrix.scaleM(model,0,b.sx,b.sy,b.sz);
            float[] mvp=new float[16]; Matrix.multiplyMM(mvp,0,proj,0,view,0); Matrix.multiplyMM(mvp,0,mvp,0,model,0);
            GLES30.glUniformMatrix4fv(uMVP,1,false,mvp,0); GLES30.glUniform4f(uC,b.r,b.g,b.b,1);
            for(int f=0;f<6;f++) GLES30.glDrawArrays(5,f*4,4);
        }
    }
    private void update(){
        float s=0.3f, rad=(float)Math.toRadians(rY);
        pX+=((float)Math.sin(rad)*-jY+(float)Math.cos(rad)*jX)*s;
        pZ+=((float)-Math.cos(rad)*-jY+(float)Math.sin(rad)*jX)*s;
        if(!isEngineMode){vY-=0.02f; pY+=vY; if(pY<4.2f){pY=4.2f; vY=0;}}
    }
    public void place(){ blocks.add(new Block(pX,pY,pZ-5, 2,2,2, 0,0.7f,1)); }
}
class Block { public float x,y,z,sx,sy,sz,r,g,b; Block(float x,float y,float z,float sx,float sy,float sz,float r,float g,float b){this.x=x;this.y=y;this.z=z;this.sx=sx;this.sy=sy;this.sz=sz;this.r=r;this.g=g;this.b=b;} }

package com.enjoer.game;
public class Block {
    public float x, y, z, sx, sy, sz, r, g, b;
    public Block(float x, float y, float z, float sx, float sy, float sz, float r, float g, float b) {
        this.x=x; this.y=y; this.z=z; this.sx=sx; this.sy=sy; this.sz=sz; this.r=r; this.g=g; this.b=b;
    }
    public float minX(){return x-sx/2;} public float maxX(){return x+sx/2;}
    public float minY(){return y-sy/2;} public float maxY(){return y+sy/2;}
    public float minZ(){return z-sz/2;} public float maxZ(){return z+sz/2;}
}

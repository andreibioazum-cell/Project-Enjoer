package com.enjoer.game;
import android.annotation.SuppressLint;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.Toast;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;

public class MainActivity extends AppCompatActivity {
    private GameRenderer ren;
    private float lx,ly;
    private View menuUI,gameUI,engineUI;
    private Handler handler = new Handler(Looper.getMainLooper());

    @SuppressLint("ClickableViewAccessibility")
    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        
        android.opengl.GLSurfaceView gv=findViewById(R.id.gl_view);
        gv.setEGLContextClientVersion(3);
        ren=new GameRenderer();
        gv.setRenderer(ren);

        menuUI=findViewById(R.id.menu_ui);
        gameUI=findViewById(R.id.game_ui);
        engineUI=findViewById(R.id.engine_ui);

        findViewById(R.id.btn_play).setOnClickListener(v->{
            ren.isEngineMode=false; ren.loadBuiltInLevel(); ren.pX=0;ren.pY=7;ren.pZ=15;
            menuUI.setVisibility(View.GONE); gameUI.setVisibility(View.VISIBLE);
            handler.postDelayed(() -> setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE), 50);
        });
        
        findViewById(R.id.btn_menu).setOnClickListener(v->{ menuUI.setVisibility(View.VISIBLE); gameUI.setVisibility(View.GONE); setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT); });

        setupJoy(findViewById(R.id.joy_base),findViewById(R.id.joy_stick));
        setupLook(findViewById(R.id.look_zone));

        findViewById(R.id.btn_jump).setOnTouchListener((v,e)->{
            if(e.getAction()==MotionEvent.ACTION_DOWN&&ren.isOnGround())ren.vY=.55f;
            return true;
        });
    }

    private void setupJoy(View base,View stick){base.setOnTouchListener((v,e)->{float cx=v.getWidth()/2f,cy=v.getHeight()/2f;if(e.getAction()==MotionEvent.ACTION_MOVE||e.getAction()==MotionEvent.ACTION_DOWN){float dx=e.getX()-cx,dy=e.getY()-cy,d=(float)Math.sqrt(dx*dx+dy*dy),lim=cx-stick.getWidth()/2f;if(d>lim){dx*=lim/d;dy*=lim/d;}stick.setTranslationX(dx);stick.setTranslationY(dy);ren.jX=dx/lim;ren.jY=dy/lim;}else if(e.getAction()==MotionEvent.ACTION_UP){stick.setTranslationX(0);stick.setTranslationY(0);ren.jX=0;ren.jY=0;}return true;});}
    private void setupLook(View z){z.setOnTouchListener((v,e)->{if(e.getAction()==MotionEvent.ACTION_DOWN){lx=e.getX();ly=e.getY();}else if(e.getAction()==MotionEvent.ACTION_MOVE){ren.rY+=(e.getX()-lx)*.25f;ren.rX=Math.max(-85,Math.min(85,ren.rX-(e.getY()-ly)*.25f));lx=e.getX();ly=e.getY();}return true;});}
}

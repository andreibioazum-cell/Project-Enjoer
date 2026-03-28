package com.enjoer.game;
import android.os.Bundle;
import android.view.View;
import androidx.appcompat.app.AppCompatActivity;
import android.opengl.GLSurfaceView;

public class MainActivity extends AppCompatActivity {
    private GameRenderer ren;
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        ren = new GameRenderer(this);
        GLSurfaceView gv = findViewById(R.id.gl_view);
        gv.setEGLContextClientVersion(3);
        gv.setRenderer(ren);

        findViewById(R.id.btn_play).setOnClickListener(v -> {
            findViewById(R.id.menu_ui).setVisibility(View.GONE);
            findViewById(R.id.game_ui).setVisibility(View.VISIBLE);
        });
    }
}

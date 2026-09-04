package com.cb4;

import android.app.NativeActivity;
import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * NativeActivity мессенджера с мостом к системной клавиатуре.
 * Нативный код (runtime.c) держит собственный буфер ввода, а невидимый
 * EditText ниже служит только источником IME: каждое изменение текста
 * зеркалится в C через nativeReplaceText, Enter — через nativeSubmitText.
 * Имена JNI-методов фиксированы (Java_com_cb4_GameActivity_*), поэтому
 * пакет/класс переименовывать нельзя.
 */
public class GameActivity extends NativeActivity {

    private EditText editor;
    private boolean editorActive = false;
    private boolean syncing = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();

        FrameLayout root = new FrameLayout(this);
        editor = new EditText(this);
        editor.setLayoutParams(new FrameLayout.LayoutParams(1, 1));
        editor.setAlpha(0.0f);
        editor.setBackgroundColor(0x00000000);
        editor.setImeOptions(EditorInfo.IME_ACTION_SEND | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        editor.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_VARIATION_NORMAL
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        editor.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, android.view.KeyEvent event) {
                nativeSubmitText();
                return true;
            }
        });
        editor.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override
            public void afterTextChanged(Editable s) {
                if (syncing) return;
                nativeReplaceText(s == null ? "" : s.toString());
            }
        });
        root.addView(editor);
        setContentView(root);
    }

    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    /* ── вызывается из нативного кода ── */

    public void showGameKeyboard(final String current) {
        editorActive = true;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                syncing = true;
                String want = current == null ? "" : current;
                if (!editor.getText().toString().equals(want)) editor.setText(want);
                editor.setSelection(editor.getText().length());
                syncing = false;
                editor.requestFocus();
                editor.setFocusableInTouchMode(true);
                InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(editor, InputMethodManager.SHOW_IMPLICIT);
                }
            }
        });
    }

    public void hideGameKeyboard() {
        editorActive = false;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) imm.hideSoftInputFromWindow(editor.getWindowToken(), 0);
                editor.clearFocus();
            }
        });
    }

    public void setGameKeyboardText(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                String want = text == null ? "" : text;
                if (editor.getText().toString().equals(want)) return;
                syncing = true;
                editor.setText(want);
                editor.setSelection(editor.getText().length());
                syncing = false;
            }
        });
    }

    public boolean gameKeyboardActive() {
        return editorActive;
    }

    /* ── нативные методы (реализованы в runtime.c) ── */
    public native void nativeReplaceText(String value);
    public native void nativeSubmitText();
    public native void nativeKeyboardHidden();
}

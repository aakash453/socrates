package com.multiverse.socrates.app

import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class InferenceActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(
            TextView(this).apply {
                text = "Socrates Engine v0.1.0\nEdge AI Runtime Ready"
                textSize = 18f
                setPadding(64, 64, 64, 64)
            }
        )
    }
}

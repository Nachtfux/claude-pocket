# Release builds are not minified (see app/build.gradle.kts). Rules kept here
# in case minification is enabled later: ONNX Runtime uses JNI reflection.
-keep class ai.onnxruntime.** { *; }

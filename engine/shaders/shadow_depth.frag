#version 460 core

// The framebuffer this draws into (see model_draw_depth()) has no color
// attachment -- gl_FragDepth is written implicitly from gl_FragCoord.z, so
// there's nothing left for this stage to do.
void main()
{
}

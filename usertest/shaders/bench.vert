#version 450
// One triangle that covers the whole target: (-1,-1), (3,-1), (-1,3) -- no vertex buffer. The benchmark's "full-screen fill".
void main()
{
   vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
   gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}

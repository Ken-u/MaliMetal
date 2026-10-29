#version 450
// The lower-left half of the target: (-1,-1), (1,-1), (-1,1) -- no vertex buffer.
void main()
{
   vec2 p[3] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0));
   gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}

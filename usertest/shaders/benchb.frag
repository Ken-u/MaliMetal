#version 450
// A half-transparent colour into target 0, for the source-over blend.
layout(location = 0) out vec4 c0;
void main()
{
   c0 = vec4(0.2, 0.1, 0.05, 0.5);
}

#version 450
// The same into targets 0 and 1.
layout(location = 0) out vec4 c0;
layout(location = 1) out vec4 c1;
void main()
{
   c0 = vec4(0.1, 0.2, 0.3, 1.0);
   c1 = vec4(0.4, 0.5, 0.6, 1.0);
}

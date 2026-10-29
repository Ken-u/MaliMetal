#version 450
// A constant colour into target 0 (the cheapest fill there is).
layout(location = 0) out vec4 c0;
void main()
{
   c0 = vec4(0.1, 0.2, 0.3, 1.0);
}

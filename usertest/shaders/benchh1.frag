#version 450
// About as much arithmetic as CoreAnimation's plain fill (Mesa: 0.7 cycles for the arithmetic pipes): a dependent chain of multiply-adds on the
// fragment's position, so it cannot be folded; into target 0.
layout(location = 0) out vec4 c0;
void main()
{
   float x = gl_FragCoord.x * 0.0009765625 + gl_FragCoord.y * 0.00048828125;
   float y = x * 0.5 + 0.25;
   for (int i = 0; i < 24; i++) {
      x = fma(x, 0.9990234375, y);
      y = fma(y, 0.5009765625, x * 0.0009765625);
   }
   c0 = vec4(fract(x), fract(y), fract(x + y), 1.0);
}

// Screen effects: colour grade (brightness, contrast, gamma, saturation).
// Same maths as the tiny_computer compositor's colour pass.
// u_setting: x = brightness (-1..1), y = contrast factor (0.5..2), z = gamma (0.1..5), w = saturation (0..2)

#ifdef GL_ES
precision mediump float;
precision mediump int;
#endif

uniform sampler2D sampler0;
varying vec2 v_texcoord0;

uniform vec4 u_setting;

void main() {
  vec3 c = texture2D(sampler0, v_texcoord0).rgb;
  c = clamp(c + vec3(u_setting.x), 0.0, 1.0);
  c = (c - vec3(0.5)) * u_setting.y + vec3(0.5);
  float grey = dot(c, vec3(0.2125, 0.7154, 0.0721));
  c = mix(vec3(grey), c, u_setting.w);
  c = pow(max(c, vec3(0.0)), vec3(1.0 / u_setting.z));
  gl_FragColor = vec4(c, 1.0);
}

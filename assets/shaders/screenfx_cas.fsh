// Screen effects: AMD FidelityFX Contrast Adaptive Sharpening (CAS), as in the tiny_computer compositor.
// u_setting.x = sharpness 0..1

#ifdef GL_ES
precision mediump float;
precision mediump int;
#endif

uniform sampler2D sampler0;
varying vec2 v_texcoord0;

uniform vec2 u_pixelDelta;
uniform vec4 u_setting;

void main() {
  vec2 px = u_pixelDelta;
  vec2 uv = v_texcoord0;
  vec3 a = texture2D(sampler0, uv + vec2(-px.x, -px.y)).rgb;
  vec3 b = texture2D(sampler0, uv + vec2(0.0, -px.y)).rgb;
  vec3 c = texture2D(sampler0, uv + vec2(px.x, -px.y)).rgb;
  vec3 d = texture2D(sampler0, uv + vec2(-px.x, 0.0)).rgb;
  vec3 e = texture2D(sampler0, uv).rgb;
  vec3 f = texture2D(sampler0, uv + vec2(px.x, 0.0)).rgb;
  vec3 g = texture2D(sampler0, uv + vec2(-px.x, px.y)).rgb;
  vec3 h = texture2D(sampler0, uv + vec2(0.0, px.y)).rgb;
  vec3 i = texture2D(sampler0, uv + vec2(px.x, px.y)).rgb;

  vec3 mnRGB = min(min(min(d, e), min(f, b)), h);
  vec3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, i)));
  mnRGB += mnRGB2;

  vec3 mxRGB = max(max(max(d, e), max(f, b)), h);
  vec3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, i)));
  mxRGB += mxRGB2;

  vec3 rcpMRGB = vec3(1.0) / max(mxRGB, vec3(0.0001));
  vec3 ampRGB = clamp(min(mnRGB, vec3(2.0) - mxRGB) * rcpMRGB, 0.0, 1.0);
  ampRGB = inversesqrt(max(ampRGB, vec3(0.0001)));

  float peak = 8.0 - 3.0 * u_setting.x;
  vec3 wRGB = vec3(-1.0) / (ampRGB * peak);
  vec3 rcpWeightRGB = vec3(1.0) / (vec3(1.0) + 4.0 * wRGB);

  vec3 window = b + d + f + h;
  vec3 outColor = clamp((window * wRGB + e) * rcpWeightRGB, 0.0, 1.0);
  gl_FragColor = vec4(outColor, 1.0);
}

// Screen effects: "HDR" bloom/tone effect (the classic HDR.fx look, as in the tiny_computer compositor)
// plus the terminal dither used for debanding.
// u_setting: x = HDR on (0/1), y = deband strength (0..2, in 1/255 steps)

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
precision mediump int;
#endif

uniform sampler2D sampler0;
varying vec2 v_texcoord0;

uniform vec2 u_pixelDelta;
uniform vec4 u_setting;

// Interleaved gradient noise.
float ign(vec2 p) {
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
  vec2 px = u_pixelDelta;
  vec2 uv = v_texcoord0;
  vec3 color = texture2D(sampler0, uv).rgb;

  if (u_setting.x > 0.5) {
    float radius1 = 0.793;
    float radius2 = 0.87;
    float hdrPower = 1.30;

    vec3 bloom1 = texture2D(sampler0, uv + vec2(1.5, -1.5) * radius1 * px).rgb;
    bloom1 += texture2D(sampler0, uv + vec2(-1.5, -1.5) * radius1 * px).rgb;
    bloom1 += texture2D(sampler0, uv + vec2(1.5, 1.5) * radius1 * px).rgb;
    bloom1 += texture2D(sampler0, uv + vec2(-1.5, 1.5) * radius1 * px).rgb;
    bloom1 += texture2D(sampler0, uv + vec2(0.0, -2.5) * radius1 * px).rgb;
    bloom1 += texture2D(sampler0, uv + vec2(0.0, 2.5) * radius1 * px).rgb;
    bloom1 += texture2D(sampler0, uv + vec2(-2.5, 0.0) * radius1 * px).rgb;
    bloom1 += texture2D(sampler0, uv + vec2(2.5, 0.0) * radius1 * px).rgb;
    bloom1 *= 0.005;

    vec3 bloom2 = texture2D(sampler0, uv + vec2(1.5, -1.5) * radius2 * px).rgb;
    bloom2 += texture2D(sampler0, uv + vec2(-1.5, -1.5) * radius2 * px).rgb;
    bloom2 += texture2D(sampler0, uv + vec2(1.5, 1.5) * radius2 * px).rgb;
    bloom2 += texture2D(sampler0, uv + vec2(-1.5, 1.5) * radius2 * px).rgb;
    bloom2 += texture2D(sampler0, uv + vec2(0.0, -2.5) * radius2 * px).rgb;
    bloom2 += texture2D(sampler0, uv + vec2(0.0, 2.5) * radius2 * px).rgb;
    bloom2 += texture2D(sampler0, uv + vec2(-2.5, 0.0) * radius2 * px).rgb;
    bloom2 += texture2D(sampler0, uv + vec2(2.5, 0.0) * radius2 * px).rgb;
    bloom2 *= 0.010;

    float dist = radius2 - radius1;
    vec3 hdr = (color + (bloom2 - bloom1)) * dist;
    vec3 blend = hdr + color;
    color = clamp(pow(abs(blend), vec3(hdrPower)) + hdr, 0.0, 1.0);
  }

  if (u_setting.y > 0.0) {
    vec2 p = uv / px;
    float tri = ign(p) + ign(p + vec2(11.0, 17.0)) - 1.0;
    color += vec3(tri * (u_setting.y / 255.0));
  }

  gl_FragColor = vec4(color, 1.0);
}

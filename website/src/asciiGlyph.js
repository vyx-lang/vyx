const CHARACTERS = ' .,:;-=+*#%@'
const VERTEX = `
attribute vec2 position;
varying vec2 uv;
void main() { uv = position * .5 + .5; gl_Position = vec4(position, 0., 1.); }
`
// Shade once per character, then composite the font atlas at display resolution.
const MODEL = `
precision highp float;
uniform vec2 grid;
uniform vec2 pointer;
uniform float aspect;
uniform float time;
mat2 turn(float a) { float c = cos(a), s = sin(a); return mat2(c, -s, s, c); }
float box(vec3 p, vec3 size) {
  vec3 q = abs(p) - size;
  return length(max(q, 0.)) + min(max(q.x, max(q.y, q.z)), 0.) - .065;
}
float shape(vec3 p) {
  p.y -= .055 * sin(time * .46);
  p.xz = turn(-.42 + time * .235 + pointer.x) * p.xz;
  p.yz = turn(.12 + .09 * sin(time * .29) + pointer.y) * p.yz;
  p.xy = turn(.035 * sin(time * .33)) * p.xy;
  p.x /= 1.12;
  vec3 left = p - vec3(-.67, .08, 0.);
  vec3 right = p - vec3(.67, .08, 0.);
  left.xy = turn(.51) * left.xy;
  right.xy = turn(-.51) * right.xy;
  return min(box(left, vec3(.225, 1.36, .34)), box(right, vec3(.225, 1.36, .34)));
}
void main() {
  vec2 screen = (gl_FragCoord.xy / grid - .5) * vec2(aspect, 1.);
  vec3 eye = vec3(0., 0., 4.5);
  vec3 ray = normalize(vec3(screen * 4., -4.));
  float distance = 0.;
  vec3 point;
  bool hit = false;
  for (int i = 0; i < 80; i++) {
    point = eye + ray * distance;
    float step = shape(point);
    if (step < .003) { hit = true; break; }
    distance += step * .78;
    if (distance > 8.) break;
  }
  if (!hit) { gl_FragColor = vec4(0.); return; }
  vec2 e = vec2(.006, 0.);
  vec3 normal = normalize(vec3(
    shape(point + e.xyy) - shape(point - e.xyy),
    shape(point + e.yxy) - shape(point - e.yxy),
    shape(point + e.yyx) - shape(point - e.yyx)));
  float light = max(0., dot(normal, normalize(vec3(-.7, 1.1, 1.8))));
  float density = clamp(.95 - light * .42, .4, .95);
  float edge = pow(1. - abs(dot(normal, -ray)), 3.);
  float scan = pow(max(0., 1. - abs(point.y - 1.4 * sin(time * .48)) * 7.), 3.);
  gl_FragColor = vec4(density, scan * edge * .5, edge, 1.);
}
`
const ASCII = `
precision mediump float;
varying vec2 uv;
uniform sampler2D scene;
uniform sampler2D atlas;
uniform vec2 grid;
uniform vec3 ink;
uniform vec3 accent;
uniform float characters;
void main() {
  vec2 cell = uv * grid;
  vec4 cellValue = texture2D(scene, (floor(cell) + .5) / grid);
  if (cellValue.a < .5) { gl_FragColor = vec4(0.); return; }
  float character = floor(cellValue.r * (characters - 2.)) + 1.;
  vec2 letter = fract(cell);
  float coverage = texture2D(atlas, vec2((character + letter.x) / characters, letter.y)).a;
  gl_FragColor = vec4(mix(ink, accent, cellValue.g), coverage * .92);
}
`

export function createAsciiGlyph(canvas) {
  const gl = canvas.getContext('webgl', { alpha: true, premultipliedAlpha: false, antialias: false, depth: false, powerPreference: 'low-power' })
  if (!gl) return null
  const columns = 132, rows = 58
  const shaders = [], programs = [], textures = []
  let buffer, framebuffer
  function dispose() {
    programs.forEach(value => gl.deleteProgram(value))
    shaders.forEach(value => gl.deleteShader(value))
    textures.forEach(value => gl.deleteTexture(value))
    gl.deleteBuffer(buffer)
    gl.deleteFramebuffer(framebuffer)
  }
  function program(fragment) {
    const compiled = [VERTEX, fragment].map((source, index) => {
      const shader = gl.createShader(index ? gl.FRAGMENT_SHADER : gl.VERTEX_SHADER)
      shaders.push(shader)
      gl.shaderSource(shader, source)
      gl.compileShader(shader)
      if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader))
      return shader
    })
    const result = gl.createProgram()
    programs.push(result)
    compiled.forEach(shader => gl.attachShader(result, shader))
    gl.linkProgram(result)
    if (!gl.getProgramParameter(result, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(result))
    return result
  }
  function texture() {
    const result = gl.createTexture()
    textures.push(result)
    gl.bindTexture(gl.TEXTURE_2D, result)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    return result
  }
  try {
    const model = program(MODEL), ascii = program(ASCII)
    buffer = gl.createBuffer()
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW)
    const scene = texture()
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, columns, rows, 0, gl.RGBA, gl.UNSIGNED_BYTE, null)
    framebuffer = gl.createFramebuffer()
    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer)
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, scene, 0)
    if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) throw new Error('ASCII framebuffer unavailable')
    const atlas = texture()
    const letters = document.createElement('canvas')
    letters.width = CHARACTERS.length * 20
    letters.height = 30
    const context = letters.getContext('2d')
    function refreshAtlas() {
      context.clearRect(0, 0, letters.width, letters.height)
      context.font = '500 23px "JetBrains Mono", monospace'
      context.textAlign = 'center'
      context.textBaseline = 'middle'
      context.fillStyle = '#fff'
      for (let i = 0; i < CHARACTERS.length; i++) context.fillText(CHARACTERS[i], i * 20 + 10, 15)
      gl.bindTexture(gl.TEXTURE_2D, atlas)
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true)
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, letters)
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)
    }
    refreshAtlas()
    const locations = (value, names) => Object.fromEntries(names.map(name => [name, gl.getUniformLocation(value, name)]))
    const m = locations(model, ['grid', 'pointer', 'aspect', 'time'])
    const a = locations(ascii, ['scene', 'atlas', 'grid', 'ink', 'accent', 'characters'])
    function use(value) {
      gl.useProgram(value)
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
      const position = gl.getAttribLocation(value, 'position')
      gl.enableVertexAttribArray(position)
      gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0)
    }
    function render(time, pointer, ink, accent) {
      const width = Math.max(1, canvas.clientWidth), height = Math.max(1, canvas.clientHeight)
      const ratio = Math.min(window.devicePixelRatio || 1, 1.5)
      const pixels = [Math.round(width * ratio), Math.round(height * ratio)]
      if (canvas.width !== pixels[0] || canvas.height !== pixels[1]) { canvas.width = pixels[0]; canvas.height = pixels[1] }
      use(model)
      gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer)
      gl.viewport(0, 0, columns, rows)
      gl.uniform2f(m.grid, columns, rows)
      gl.uniform2fv(m.pointer, pointer)
      gl.uniform1f(m.aspect, width / height)
      gl.uniform1f(m.time, time)
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
      use(ascii)
      gl.bindFramebuffer(gl.FRAMEBUFFER, null)
      gl.viewport(0, 0, canvas.width, canvas.height)
      gl.uniform2f(a.grid, columns, rows)
      gl.uniform3fv(a.ink, ink)
      gl.uniform3fv(a.accent, accent)
      gl.uniform1f(a.characters, CHARACTERS.length)
      gl.activeTexture(gl.TEXTURE0)
      gl.bindTexture(gl.TEXTURE_2D, scene)
      gl.uniform1i(a.scene, 0)
      gl.activeTexture(gl.TEXTURE1)
      gl.bindTexture(gl.TEXTURE_2D, atlas)
      gl.uniform1i(a.atlas, 1)
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
      gl.activeTexture(gl.TEXTURE0)
    }
    return { render, refreshAtlas, dispose }
  } catch (error) {
    dispose()
    console.warn('ASCII illustration uses its static fallback:', error.message)
    return null
  }
}

export function staticAsciiGlyph() {
  return Array.from({ length: 38 }, (_, row) => Array.from({ length: 82 }, (_, column) => {
    const x = (column - 40.5) / 21, y = (18.5 - row) / 14
    const edge = Math.abs(Math.abs(x) - (y + 1.1) * .48)
    if (y < -1.18 || y > 1.1 || edge > .2) return ' '
    return '-=+*#%@'[Math.min(6, Math.floor(edge * 20 + (x > 0 ? 1 : 2)))]
  }).join('')).join('\n')
}

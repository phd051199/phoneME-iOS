export type PresentableFrame = {
  pixels: Uint8Array<ArrayBufferLike> | Uint8ClampedArray<ArrayBufferLike>;
  width: number;
  height: number;
  generation: bigint;
};

export type FramePresenterBackend = "webgpu" | "webgl2" | "canvas2d";

export interface FramePresenter {
  readonly backend: FramePresenterBackend;
  present(frame: PresentableFrame): void;
  setFiltering(enabled: boolean): void;
  capture(): Promise<Blob | null>;
  dispose(): void;
}

type PresentCanvas = HTMLCanvasElement | OffscreenCanvas;
type WebGpuNavigator = Navigator & { gpu?: any };

type WebGpuState = {
  gpu: any;
  adapter: any;
  device: any;
  context: any;
  format: string;
  pipeline: any;
  nearestSampler: any;
  linearSampler: any;
};

const WEBGPU_SHADER = `
@group(0) @binding(0) var frameSampler: sampler;
@group(0) @binding(1) var frameTexture: texture_2d<f32>;

struct VertexOutput {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vertexMain(@builtin(vertex_index) vertexIndex: u32) -> VertexOutput {
  let positions = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0),
    vec2<f32>( 3.0, -1.0),
    vec2<f32>(-1.0,  3.0)
  );
  let position = positions[vertexIndex];
  var output: VertexOutput;
  output.position = vec4<f32>(position, 0.0, 1.0);
  output.uv = vec2<f32>((position.x + 1.0) * 0.5, (1.0 - position.y) * 0.5);
  return output;
}

@fragment
fn fragmentMain(input: VertexOutput) -> @location(0) vec4<f32> {
  return textureSample(frameTexture, frameSampler, input.uv);
}
`;

const WEBGL_VERTEX_SHADER = `#version 300 es
precision highp float;
out vec2 vUv;
void main() {
  vec2 positions[3] = vec2[3](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
  );
  vec2 position = positions[gl_VertexID];
  gl_Position = vec4(position, 0.0, 1.0);
  vUv = vec2((position.x + 1.0) * 0.5, (1.0 - position.y) * 0.5);
}
`;

const WEBGL_FRAGMENT_SHADER = `#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uFrame;
out vec4 outColor;
void main() {
  outColor = texture(uFrame, vUv);
}
`;

function resizeCanvas(canvas: PresentCanvas, width: number, height: number) {
  // Only assigning when dimensions actually change avoids reallocating the
  // drawing buffer while keeping it matched to the guest framebuffer. The CSS
  // layer handles visual scaling; leaving an untouched canvas at its 300x150
  // default causes an unnecessary extra resample and wrong aspect ratio.
  if (canvas.width !== width) canvas.width = width;
  if (canvas.height !== height) canvas.height = height;
}

async function captureCanvas(canvas: PresentCanvas): Promise<Blob | null> {
  if (typeof OffscreenCanvas !== "undefined" && canvas instanceof OffscreenCanvas) {
    return await canvas.convertToBlob({ type: "image/png" });
  }
  if (typeof HTMLCanvasElement !== "undefined" && canvas instanceof HTMLCanvasElement) {
    return await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/png"));
  }
  return null;
}

class WebGpuFramePresenter implements FramePresenter {
  readonly backend = "webgpu" as const;
  private texture: any = null;
  private bindGroup: any = null;
  private textureWidth = 0;
  private textureHeight = 0;
  private filtering = false;

  private constructor(
    private readonly canvas: PresentCanvas,
    private readonly state: WebGpuState
  ) {}

  static async create(canvas: PresentCanvas): Promise<WebGpuFramePresenter | null> {
    const navigatorValue = globalThis.navigator as WebGpuNavigator | undefined;
    const gpu = navigatorValue?.gpu;
    if (!gpu) return null;

    const adapter = await gpu.requestAdapter({ powerPreference: "low-power" });
    if (!adapter) return null;
    const device = await adapter.requestDevice();
    const format = gpu.getPreferredCanvasFormat();

    let context: any;
    try {
      context = canvas.getContext("webgpu" as any);
    } catch {
      device.destroy?.();
      return null;
    }
    if (!context) {
      device.destroy?.();
      return null;
    }
    context.configure({ device, format, alphaMode: "opaque" });

    const shader = device.createShaderModule({ code: WEBGPU_SHADER });
    const pipeline = device.createRenderPipeline({
      layout: "auto",
      vertex: { module: shader, entryPoint: "vertexMain" },
      fragment: {
        module: shader,
        entryPoint: "fragmentMain",
        targets: [{ format }]
      },
      primitive: { topology: "triangle-list" }
    });
    const nearestSampler = device.createSampler({
      magFilter: "nearest",
      minFilter: "nearest",
      mipmapFilter: "nearest"
    });
    const linearSampler = device.createSampler({
      magFilter: "linear",
      minFilter: "linear",
      mipmapFilter: "nearest"
    });

    return new WebGpuFramePresenter(canvas, {
      gpu,
      adapter,
      device,
      context,
      format,
      pipeline,
      nearestSampler,
      linearSampler
    });
  }

  setFiltering(enabled: boolean) {
    if (this.filtering === enabled) return;
    this.filtering = enabled;
    if (this.texture) this.rebuildBindGroup();
  }

  present(frame: PresentableFrame) {
    const { device, context, pipeline } = this.state;
    resizeCanvas(this.canvas, frame.width, frame.height);
    this.ensureTexture(frame.width, frame.height);

    device.queue.writeTexture(
      { texture: this.texture },
      frame.pixels,
      { bytesPerRow: frame.width * 4, rowsPerImage: frame.height },
      { width: frame.width, height: frame.height, depthOrArrayLayers: 1 }
    );

    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: context.getCurrentTexture().createView(),
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
        loadOp: "clear",
        storeOp: "store"
      }]
    });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, this.bindGroup);
    pass.draw(3, 1, 0, 0);
    pass.end();
    device.queue.submit([encoder.finish()]);
  }

  async capture() {
    return await captureCanvas(this.canvas);
  }

  dispose() {
    try { this.texture?.destroy?.(); } catch { /* best effort */ }
    this.texture = null;
    this.bindGroup = null;
    try { this.state.context.unconfigure?.(); } catch { /* best effort */ }
    try { this.state.device.destroy?.(); } catch { /* best effort */ }
  }

  private ensureTexture(width: number, height: number) {
    if (this.texture && this.textureWidth === width && this.textureHeight === height) return;
    try { this.texture?.destroy?.(); } catch { /* best effort */ }
    const usage = (globalThis as typeof globalThis & { GPUTextureUsage?: { COPY_DST: number; TEXTURE_BINDING: number } }).GPUTextureUsage;
    if (!usage) throw new Error("WebGPU texture usage constants are unavailable");
    this.texture = this.state.device.createTexture({
      size: { width, height, depthOrArrayLayers: 1 },
      format: "rgba8unorm",
      usage: usage.COPY_DST | usage.TEXTURE_BINDING
    });
    this.textureWidth = width;
    this.textureHeight = height;
    this.rebuildBindGroup();
  }

  private rebuildBindGroup() {
    const sampler = this.filtering ? this.state.linearSampler : this.state.nearestSampler;
    this.bindGroup = this.state.device.createBindGroup({
      layout: this.state.pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: sampler },
        { binding: 1, resource: this.texture.createView() }
      ]
    });
  }
}

class WebGl2FramePresenter implements FramePresenter {
  readonly backend = "webgl2" as const;
  private readonly texture: WebGLTexture;
  private readonly program: WebGLProgram;
  private readonly vao: WebGLVertexArrayObject;
  private textureWidth = 0;
  private textureHeight = 0;
  private filtering = false;

  private constructor(
    private readonly canvas: PresentCanvas,
    private readonly gl: WebGL2RenderingContext,
    texture: WebGLTexture,
    program: WebGLProgram,
    vao: WebGLVertexArrayObject
  ) {
    this.texture = texture;
    this.program = program;
    this.vao = vao;
  }

  static create(canvas: PresentCanvas): WebGl2FramePresenter | null {
    let gl: WebGL2RenderingContext | null;
    try {
      gl = canvas.getContext("webgl2", {
        alpha: false,
        antialias: false,
        depth: false,
        stencil: false,
        desynchronized: true,
        preserveDrawingBuffer: false,
        powerPreference: "low-power"
      } as WebGLContextAttributes) as WebGL2RenderingContext | null;
    } catch {
      return null;
    }
    if (!gl) return null;

    const vertexShader = compileShader(gl, gl.VERTEX_SHADER, WEBGL_VERTEX_SHADER);
    const fragmentShader = compileShader(gl, gl.FRAGMENT_SHADER, WEBGL_FRAGMENT_SHADER);
    const program = gl.createProgram();
    const texture = gl.createTexture();
    const vao = gl.createVertexArray();
    if (!program || !texture || !vao) {
      gl.deleteShader(vertexShader);
      gl.deleteShader(fragmentShader);
      if (program) gl.deleteProgram(program);
      if (texture) gl.deleteTexture(texture);
      if (vao) gl.deleteVertexArray(vao);
      return null;
    }

    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);
    gl.deleteShader(vertexShader);
    gl.deleteShader(fragmentShader);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const message = gl.getProgramInfoLog(program) || "WebGL2 program link failed";
      gl.deleteProgram(program);
      gl.deleteTexture(texture);
      gl.deleteVertexArray(vao);
      throw new Error(message);
    }

    gl.useProgram(program);
    gl.bindVertexArray(vao);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.uniform1i(gl.getUniformLocation(program, "uFrame"), 0);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.disable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);

    const presenter = new WebGl2FramePresenter(canvas, gl, texture, program, vao);
    presenter.applyFiltering();
    return presenter;
  }

  setFiltering(enabled: boolean) {
    if (this.filtering === enabled) return;
    this.filtering = enabled;
    this.applyFiltering();
  }

  present(frame: PresentableFrame) {
    const gl = this.gl;
    resizeCanvas(this.canvas, frame.width, frame.height);
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.useProgram(this.program);
    gl.bindVertexArray(this.vao);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texture);

    if (this.textureWidth !== frame.width || this.textureHeight !== frame.height) {
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA,
        frame.width,
        frame.height,
        0,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        null
      );
      this.textureWidth = frame.width;
      this.textureHeight = frame.height;
    }

    gl.texSubImage2D(
      gl.TEXTURE_2D,
      0,
      0,
      0,
      frame.width,
      frame.height,
      gl.RGBA,
      gl.UNSIGNED_BYTE,
      frame.pixels
    );
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  async capture() {
    return await captureCanvas(this.canvas);
  }

  dispose() {
    this.gl.deleteTexture(this.texture);
    this.gl.deleteProgram(this.program);
    this.gl.deleteVertexArray(this.vao);
  }

  private applyFiltering() {
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    const filter = this.filtering ? gl.LINEAR : gl.NEAREST;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
  }
}

class Canvas2dFramePresenter implements FramePresenter {
  readonly backend = "canvas2d" as const;
  private scratch: Uint8ClampedArray<ArrayBuffer> | null = null;
  private imageData: ImageData | null = null;
  private width = 0;
  private height = 0;

  constructor(
    private readonly canvas: PresentCanvas,
    private readonly context: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D
  ) {}

  setFiltering(enabled: boolean) {
    this.context.imageSmoothingEnabled = enabled;
  }

  present(frame: PresentableFrame) {
    resizeCanvas(this.canvas, frame.width, frame.height);
    if (!this.imageData || this.width !== frame.width || this.height !== frame.height) {
      this.scratch = new Uint8ClampedArray(frame.width * frame.height * 4);
      this.imageData = new ImageData(this.scratch, frame.width, frame.height);
      this.width = frame.width;
      this.height = frame.height;
    }
    this.scratch!.set(frame.pixels);
    this.context.putImageData(this.imageData, 0, 0);
  }

  async capture() {
    return await captureCanvas(this.canvas);
  }

  dispose() {
    this.scratch = null;
    this.imageData = null;
  }
}

function compileShader(gl: WebGL2RenderingContext, type: number, source: string) {
  const shader = gl.createShader(type);
  if (!shader) throw new Error("Không tạo được WebGL2 shader");
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) || "WebGL2 shader compile failed";
    gl.deleteShader(shader);
    throw new Error(message);
  }
  return shader;
}

export async function createFramePresenter(
  canvas: PresentCanvas,
  filtering = false
): Promise<FramePresenter> {
  try {
    const webGpu = await WebGpuFramePresenter.create(canvas);
    if (webGpu) {
      webGpu.setFiltering(filtering);
      return webGpu;
    }
  } catch {
    // A canvas that has not committed to WebGPU yet can still fall back below.
  }

  const webGl2 = WebGl2FramePresenter.create(canvas);
  if (webGl2) {
    webGl2.setFiltering(filtering);
    return webGl2;
  }

  const context = canvas.getContext("2d", {
    alpha: false,
    desynchronized: true
  } as CanvasRenderingContext2DSettings) as CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D | null;
  if (!context) throw new Error("Trình duyệt không cung cấp backend render Canvas/WebGL/WebGPU");
  const canvas2d = new Canvas2dFramePresenter(canvas, context);
  canvas2d.setFiltering(filtering);
  return canvas2d;
}

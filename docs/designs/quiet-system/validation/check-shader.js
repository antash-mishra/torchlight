/* Numeric shader readback and GPU resource checks for the design preview. */
async (page) => {
  await page.emulateMedia({reducedMotion:'reduce'});
  return await page.evaluate(() => {
    const surface = document.createElement('div');
    surface.style.cssText = 'position:fixed;left:-10000px;top:0;width:96px;height:32px';
    const canvas = document.createElement('canvas'); surface.append(canvas); document.body.append(surface);
    const gl = canvas.getContext('webgl', {alpha:true,premultipliedAlpha:true,antialias:false,depth:false,stencil:false});
    if (!gl) throw new Error('WebGL unavailable for numeric validation');
    const live = {Program:new Set(),Buffer:new Set(),Shader:new Set()};
    for (const kind of Object.keys(live)) {
      const create=gl['create'+kind].bind(gl), remove=gl['delete'+kind].bind(gl);
      gl['create'+kind] = (...args) => { const item=create(...args); if(item) live[kind].add(item); return item; };
      gl['delete'+kind] = item => { live[kind].delete(item); return remove(item); };
    }
    let art = SearchArt.create(canvas);
    const program = gl.getParameter(gl.CURRENT_PROGRAM);
    if (!program || !gl.getProgramParameter(program,gl.LINK_STATUS)) throw new Error('Shader did not link');
    const debug = gl.getExtension('WEBGL_debug_renderer_info');
    const environment = {
      renderer: debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
      vendor: debug ? gl.getParameter(debug.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
      version:gl.getParameter(gl.VERSION), shading:gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      timerQuery:!!gl.getExtension('EXT_disjoint_timer_query'), userAgent:navigator.userAgent,
    };
    const frames = [];
    const capture = (name,time,width=96,height=32,variant=0) => {
      art.setVariant(variant);
      canvas.width=width;canvas.height=height;art.draw(time);
      const pixels=new Uint8Array(width*height*4);
      gl.readPixels(0,0,width,height,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
      const error=gl.getError(); if(error) throw new Error('GL error '+error);
      frames.push({name,time,width,height,variant,pixels:Array.from(pixels)});
      return pixels;
    };
    const original=capture('static',0);
    capture('moving',8);
    capture('repeat',0);
    capture('scaled',0,288,96);
    capture('static-again',0);
    for (const variant of [1,2]) {
      capture(`variant-${variant}-static`,0,96,32,variant);
      capture(`variant-${variant}-moving`,8,96,32,variant);
      capture(`variant-${variant}-repeat`,0,96,32,variant);
      capture(`variant-${variant}-scaled`,0,288,96,variant);
    }
    capture('restore-default',0);
    const read = () => { const p=new Uint8Array(96*32*4);gl.readPixels(0,0,96,32,gl.RGBA,gl.UNSIGNED_BYTE,p);return Array.from(p); };
    const strength=gl.getUniformLocation(program,'u_strength'), time=gl.getUniformLocation(program,'u_time'), resolution=gl.getUniformLocation(program,'u_resolution');
    gl.uniform1f(strength,0);gl.drawArrays(gl.TRIANGLES,0,3);
    frames.push({name:'disabled',time:0,width:96,height:32,pixels:read()});
    art.draw(8);gl.uniform1f(time,0);gl.drawArrays(gl.TRIANGLES,0,3);
    frames.push({name:'frozen-time-fault',time:8,width:96,height:32,pixels:read()});
    art.draw(0);gl.uniform2f(resolution,1,1);gl.drawArrays(gl.TRIANGLES,0,3);
    frames.push({name:'resolution-fault',time:0,width:96,height:32,pixels:read()});
    art.draw(0);
    const cpuDispatch=[];
    for(let i=0;i<20;i++)art.draw(i/30);
    for(let i=0;i<60;i++){const start=performance.now();art.draw(i/30);cpuDispatch.push(performance.now()-start);}
    art.destroy();art.destroy();
    // Recompile the actual field kernel behind a diagnostic grayscale output.
    // This only changes the test program; the normal preview remains untouched.
    const rawSource = gl.shaderSource.bind(gl);
    const diagnostic = SEARCH_ART_FRAGMENT.replace('void main()', 'void artMain()') + `
void main() {
  vec2 uv = gl_FragCoord.xy / u_resolution;
  float value = flow((uv - 0.5) * 3.0);
  gl_FragColor = vec4(vec3(0.5 + 0.5 * value), 1.0);
}`;
    gl.shaderSource = (shader,code) => rawSource(shader,code===SEARCH_ART_FRAGMENT ? diagnostic : code);
    art=SearchArt.create(canvas);
    capture('field-kernel',0);
    art.destroy();
    gl.shaderSource = (shader,code) => rawSource(shader,code===SEARCH_ART_FRAGMENT ? diagnostic.replace('float amplitude = 0.52;', 'float amplitude = 0.0;') : code);
    art=SearchArt.create(canvas);
    capture('field-kernel-fault',0);
    art.destroy();gl.shaderSource=rawSource;
    const cycles=[];
    for(let i=0;i<8;i++){
      art=SearchArt.create(canvas);art.draw(i);art.setVisible(false);art.setVisible(true);art.destroy();
      cycles.push({programs:live.Program.size,buffers:live.Buffer.size,shaders:live.Shader.size,error:gl.getError()});
    }
    const source = gl.shaderSource.bind(gl);
    gl.shaderSource = (shader,code) => source(shader, gl.getShaderParameter(shader,gl.SHADER_TYPE)===gl.FRAGMENT_SHADER ? 'intentional-invalid-shader' : code);
    art=SearchArt.create(canvas);
    const badCompile={fallback:canvas.hidden && surface.dataset.art==='fallback',frame:art.frame};
    art.destroy();gl.shaderSource=source;
    const originalGetContext=canvas.getContext.bind(canvas);
    canvas.getContext=()=>null;
    art=SearchArt.create(canvas);
    const unavailable={fallback:canvas.hidden,frame:art.frame};art.destroy();canvas.getContext=originalGetContext;
    const finalObjects={programs:live.Program.size,buffers:live.Buffer.size,shaders:live.Shader.size,error:gl.getError()};
    surface.remove();
    return {environment,frames,cycles,badCompile,unavailable,finalObjects,cpuDispatch};
  });
}

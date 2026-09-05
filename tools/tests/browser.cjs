/* Optional real-browser integration. Run against an isolated --storage preview. */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {chromium}=require('playwright');
const url=process.env.TEST_URL||'http://127.0.0.1:8091';
const directory=process.env.TEST_OUTPUT||'build-tests/half-voxel';
const save=process.env.TEST_SAVE||`${directory}/browser-data/world.edits`;
let browser,page;
(async()=>{
  browser=await chromium.launch({headless:true,executablePath:process.env.CHROMIUM_EXECUTABLE_PATH||undefined,
    args:JSON.parse(process.env.CHROMIUM_ARGUMENTS||'[]')});
  page=await browser.newPage({viewport:{width:1280,height:720},deviceScaleFactor:1,hasTouch:true});
  const errors=[],events=[],sizes=[];let frames=0;
  page.on('pageerror',error=>errors.push(error.message));
  page.on('request',request=>{if(request.url().endsWith('/event'))for(const line of(request.postData()||'').split('\n'))events.push(Object.fromEntries(new URLSearchParams(line)))});
  page.on('response',response=>{if(response.url().endsWith('/frame.jpg')&&response.status()===200){frames++;sizes.push(Number(response.headers()['content-length']))}});
  await page.goto(url);
  const state=()=>page.evaluate(()=>fetch('/info').then(r=>r.json()));
  const flush=()=>page.waitForFunction(()=>!sending&&queue.length===0);
  await page.waitForFunction(async()=>{const s=await fetch('/info').then(r=>r.json());return s.pending===0&&s.distance>=96},{},{timeout:20000});
  assert.match(await page.title(),/блочный мир/);
  assert.equal(await page.locator('h1,.hint').count(),0);
  assert.equal(await page.evaluate(()=>document.documentElement.scrollHeight<=innerHeight),true);
  const box=await page.locator('canvas').boundingBox();assert.equal(box.width,1280);assert.equal(box.height,720);
  const xy=(x,y)=>({x:box.x+x/960*box.width,y:box.y+y/540*box.height});
  const point=(id,x,y)=>({id,...xy(x,y),radiusX:5,radiusY:5,force:1});
  const cdp=await page.context().newCDPSession(page);
  const touch=(type,points)=>cdp.send('Input.dispatchTouchEvent',{type,touchPoints:points});
  const waitJump=value=>page.waitForFunction(v=>{const p=ctx.getImageData(912,458,1,1).data;return(p[0]>245&&p[1]>245&&p[2]>245)===v},value);
  await waitJump(true);
  assert.equal(await page.evaluate(()=>{const p=ctx.getImageData(480,270,1,1).data;return p[0]>235&&p[1]>235&&p[2]>235}),true);
  await page.screenshot({path:`${directory}/browser-walk.png`});
  const jump=point(11,882,458),joy=point(21,86,454),forward=point(21,86,394);
  const look=point(31,600,220),lookUp=point(31,630,140),flight=point(41,746,40);
  await touch('touchStart',[jump]);
  await page.waitForFunction(async()=>(await fetch('/info').then(r=>r.json())).y>13.1);
  await touch('touchStart',[jump,joy]);await touch('touchStart',[jump,joy,look]);
  await touch('touchMove',[jump,forward,lookUp]);await touch('touchStart',[jump,forward,lookUp,flight]);
  await touch('touchEnd',[flight]);await flush();await waitJump(false);
  assert.equal((await state()).flying,1);assert.equal(await page.evaluate(()=>pointers.size),3);
  await page.waitForFunction(()=>{const p=ctx.getImageData(86,420,1,1).data;return p[0]<12&&p[1]<12&&p[2]<12});
  await page.screenshot({path:`${directory}/browser-flight.png`});
  await touch('touchEnd',[]);await flush();
  const hover=await state();await page.waitForTimeout(250);const after=await state();
  for(const key of ['x','y','z'])assert.equal(after[key],hover[key]);
  await touch('touchStart',[point(51,746,40)]);await touch('touchCancel',[]);await flush();assert.equal((await state()).flying,1);
  assert.ok(events.some(e=>e.t==='cancel-pointer'));
  await page.keyboard.press('f');await flush();await waitJump(true);
  await touch('touchStart',[point(61,746,40)]);await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await flush();
  assert.equal((await state()).flying,0);await touch('touchCancel',[]);
  // Look straight down, then edit one quarter of the top face under the crosshair.
  await touch('touchStart',[point(71,600,90)]);await touch('touchMove',[point(71,600,475)]);await touch('touchEnd',[]);await flush();
  await page.waitForFunction(async()=>{const s=await fetch('/info').then(r=>r.json());return s.target&&Math.abs(s.y-Math.round(s.y*2)/2)<.001});
  const before=(await state()).target;
  const click=async(x,y,button='left')=>{const p=xy(x,y);await page.mouse.click(p.x,p.y,{button});await flush()};
  await click(696,464);
  await page.waitForFunction(async old=>JSON.stringify((await fetch('/info').then(r=>r.json())).target)!==JSON.stringify(old),before);
  await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await flush();
  function savedCell(sx,sy,sz){
    const bytes=fs.readFileSync(save);assert.equal(bytes.toString('ascii',0,7),'EJVOX01');
    for(let i=0;i<bytes.readUInt32LE(12);i++){
      const at=20+i*20,x=bytes.readInt32LE(at),y=bytes.readInt32LE(at+4),z=bytes.readInt32LE(at+8);
      if(x===Math.floor(sx/2)&&y===Math.floor(sy/2)&&z===Math.floor(sz/2))return [...bytes.subarray(at+12,at+20)];
    }
    throw Error('Expected a persisted parent-block patch');
  }
  const cells=savedCell(before.x,before.y,before.z);assert.equal(cells.filter(x=>x===0).length,1);
  await page.screenshot({path:`${directory}/browser-broken-piece.png`});
  await click(458,504);assert.equal((await state()).selected,2); // stone
  await click(600,300,'right');
  await page.waitForFunction(async old=>{const h=(await fetch('/info').then(r=>r.json())).target;return h&&h.x===old.x&&h.y===old.y&&h.z===old.z&&h.block===3},before);
  await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await flush();
  const replaced=savedCell(before.x,before.y,before.z);assert.equal(replaced.filter(x=>x===0).length,0);assert.equal(replaced.filter(x=>x===3).length,1);
  await page.screenshot({path:`${directory}/browser-placed-piece.png`});
  await page.keyboard.press('Digit6');await flush();assert.equal((await state()).selected,5);
  const time=Date.now(),start=frames;await page.waitForTimeout(2200);
  const observed=(frames-start)*1000/(Date.now()-time),fps=(await state()).fps;
  assert.ok(observed>0&&fps>0&&Math.abs(fps-observed)<Math.max(8,observed*.4));
  assert.ok(sizes.reduce((a,b)=>a+b,0)/sizes.length<960*540); // much less than raw RGBA
  // Browser chord: placing must remain possible during a left-button camera drag.
  const n=events.filter(e=>e.t==='key'&&e.k==='place'&&e.d==='1').length;
  let p=xy(600,240);await page.mouse.move(p.x,p.y);await page.mouse.down();p=xy(640,250);await page.mouse.move(p.x,p.y);
  await page.mouse.down({button:'right'});await page.mouse.up({button:'right'});await page.mouse.up();await flush();
  assert.ok(events.filter(e=>e.t==='key'&&e.k==='place'&&e.d==='1').length>n);
  await page.setViewportSize({width:844,height:390});await page.waitForFunction(()=>cv.getBoundingClientRect().width<=innerWidth+.01&&cv.getBoundingClientRect().height<=innerHeight+.01);let bounds=await page.locator('canvas').boundingBox();assert.ok(bounds.width>bounds.height&&bounds.x>=0&&bounds.y>=0);
  await page.setViewportSize({width:390,height:844});await page.waitForFunction(()=>cv.getBoundingClientRect().width<=innerWidth+.01&&cv.getBoundingClientRect().height<=innerHeight+.01);bounds=await page.locator('canvas').boundingBox();assert.ok(bounds.width>bounds.height&&bounds.width<=390&&bounds.height<=844);
  assert.ok(new Set(events.filter(e=>e.t==='down').map(e=>e.id)).size>=4);assert.deepEqual(errors,[]);
  console.log(`PASS Chromium: ${events.length} ordered events, four-finger move/look/jump/flight, cancel/blur, half-piece break/place/save, hotbar, mouse chord and landscape layouts.`);
  console.log(`Measured stream ${observed.toFixed(1)} FPS; HUD ${fps.toFixed(1)} FPS; JPEG mean ${Math.round(sizes.reduce((a,b)=>a+b,0)/sizes.length)} bytes vs 2073600 raw; no JS errors.`);
  await browser.close();
})().catch(async error=>{console.error(error);if(page)await page.screenshot({path:`${directory}/browser-failure.png`}).catch(()=>{});if(browser)await browser.close();process.exitCode=1});

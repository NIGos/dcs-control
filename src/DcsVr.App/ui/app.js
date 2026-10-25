'use strict';
const $ = id => document.getElementById(id);
const pages = [
  ['overview','Overview','What runs when you press Launch DCS, and how your last flight went.','Status at a glance','M3.5 16.5a8.5 8.5 0 1 1 17 0 M12 16.5l4.2-5.8 M6.3 11.4l1.1.7 M12 8.2v1.3 M17.7 11.4l-1.1.7 M13.3 16.5a1.3 1.3 0 1 1-2.6 0 1.3 1.3 0 0 1 2.6 0Z M5 20h14'],
  ['guide','Feature guide','What each feature does, what to expect from it and what it costs, at a glance.','What to expect','M5 4h10l4 4v12H5Z M15 4v4h4 M8 12h8 M8 15.5h8 M8 9h4'],
  ['engine','Engine Optimizations','Makes DCS itself lighter on the CPU, on a monitor or in VR: more FPS, the same image.','CPU work inside DCS','M12 8.8a3.2 3.2 0 1 0 0 6.4 3.2 3.2 0 0 0 0-6.4Z M12 3v2.6 M12 18.4V21 M3 12h2.6 M18.4 12H21 M5.6 5.6l1.9 1.9 M16.5 16.5l1.9 1.9 M5.6 18.4l1.9-1.9 M16.5 7.5l1.9-1.9','Engine'],
  ['boost','CPU Boost','Gives DCS priority and the best CPU cores, and frees video memory and desktop resources while you fly.','CPU, VRAM & monitor','M6 6h12v12H6Z M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3 M12.8 8.5 10.2 12.3h3.6l-2.6 3.7'],
  ['osd','In flight','Every in-flight key, and the status panel in the headset.','Keys & headset panel','M3 5h18v11H3Z M8 20h8 M12 16v4 M6 8.5h6 M6 11.5h9'],
  ['foveation','Quad Views','Full detail where you look, lower resolution around it: less work for the GPU. Periphery stability calms the periphery.','Foveated rendering','M3 8V5a2 2 0 0 1 2-2h3 M16 3h3a2 2 0 0 1 2 2v3 M21 16v3a2 2 0 0 1-2 2h-3 M8 21H5a2 2 0 0 1-2-2v-3 M6 12c1.6-2.7 3.6-4 6-4s4.4 1.3 6 4c-1.6 2.7-3.6 4-6 4s-4.4-1.3-6-4Z M14 12a2 2 0 1 1-4 0 2 2 0 0 1 4 0Z'],
  ['dlss','DLSS','DLSS 5, the DLSS models DCS upscales with, and Foveated Super Resolution.','Neural rendering','M10 3c.6 3.8 2.2 5.4 6 6-3.8.6-5.4 2.2-6 6-.6-3.8-2.2-5.4-6-6 3.8-.6 5.4-2.2 6-6Z M18 14c.3 1.8 1.1 2.6 3 3-1.9.4-2.7 1.2-3 3-.3-1.8-1.1-2.6-3-3 1.9-.4 2.7-1.2 3-3Z'],
  ['framegen','Framegen','Generated frames between the ones DCS renders, and the DCS frame limit that goes with them.','Generation & limit','M3 10h11v10H3Z M6.5 10V6.5h11v10H14 M10 6.5V3h11v10h-3.5'],
  ['pupil','Perception','Eye distance, Pupil shift, Depth boost and Far depth.','Eyes & depth','M2.5 12c2.2-3.7 5.4-5.5 9.5-5.5s7.3 1.8 9.5 5.5c-2.2 3.7-5.4 5.5-9.5 5.5S4.7 15.7 2.5 12Z M14.5 12a2.5 2.5 0 1 1-5 0 2.5 2.5 0 0 1 5 0Z M17 3.5l2.5 2.5L17 8.5 M19.5 6h-5'],
  ['visual','Visual realism','How the image looks in the headset: color, night vision, glare, eye-focus blur and G vision. Off by default.','Color, night & G','M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Z M12 3v18 M12 7h5 M12 11h7 M12 15h6'],
  ['setup','DCS & headset','Where DCS is installed, the DCS launcher and, in VR, the headset software.','Paths & launcher','M3 9a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2h-4.2l-1.8-2.4h-2L9.2 17H5a2 2 0 0 1-2-2Z M9.5 12a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0Z M17.5 12a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0Z'],
  ['diagnostics','Checks','What to fix before launching, what to check yourself, and the files Launch DCS writes.','Checks & files','M9 3.5h6v3H9Z M9 5H6.5A1.5 1.5 0 0 0 5 6.5v13A1.5 1.5 0 0 0 6.5 21h11a1.5 1.5 0 0 0 1.5-1.5v-13A1.5 1.5 0 0 0 17.5 5H15 M8.5 13.5l2.5 2.5 4.5-5'],
  ['support','Help & support','When something does not work: what the headset panel messages mean, a package for your report, and app updates.','Problems, reports, updates','M12 3.5a8.5 8.5 0 1 0 0 17 8.5 8.5 0 0 0 0-17Z M9.6 9.4a2.5 2.5 0 1 1 3.4 2.3c-.6.3-1 .8-1 1.5v.6 M12 16.4v.2'],
  ['recovery','Stock DCS','Undo everything DCS Control changed in DCS, in one step.','Undo every change','M4.5 12a7.5 7.5 0 1 0 2.2-5.3L4.5 9 M4.5 4.5V9H9 M12 8.5v3.5l2.5 1.8']
];
/** Pages of the VR features, hidden in Optimizations only (no VR). */
const VR_PAGES = ['foveation','dlss','framegen','pupil','visual'];
/** Ctrl shortcuts by page, fixed so pilots keep them when the rail order changes. */
const PAGE_KEYS = {overview:['Ctrl','1'],guide:['Ctrl','2'],engine:['Ctrl','3'],boost:['Ctrl','4'],foveation:['Ctrl','5'],dlss:['Ctrl','6'],framegen:['Ctrl','7'],pupil:['Ctrl','8'],visual:['Ctrl','9'],osd:['Ctrl','0'],diagnostics:['Ctrl','Shift','C']};
// The feature checklist in the right panel; each feature has its own page. First what every pilot can use (monitor or any
// headset), then the VR features for Pimax (and Sboys).
const FEATURES = [
  ['engine','Engine Optimizations','CPU work inside DCS','engine'],
  ['boost','CPU Boost','CPU scheduling','boost'],
  ['quad','Quad Views','foveated rendering · VR','foveation'],
  ['dlss','DLSS 5','neural rendering · VR','dlss'],
  ['framegen','Frame generation','generated frames · VR','framegen']
];
/** The engine optimizations a profile turns on (Engine Optimizations page). */
const ENGINE_PARTS = ['engineShaderTimeCache','enginePartitionBoost','engineCostWeights','engineShadowInstancing','engineFrameHeap','engineTaskQueueClock','engineModelAllocator','engineTextureDedupe','engineEffectBufferSkip','enginePlainCounter','engineStateFilter','engineShadowRecorder','engineGBufferRecorder'];
let state, profile, dcs = '', options = '', page = 'overview', busy = false, plan = null;
let sequence = 0, toastTimer, searchResults = [], lastProvider = 'QuadViewsFoveated', lastFrameGen = 'Nvidia', savedDraftJson = null, draftTimer, flight = null;
const pending = new Map(), fields = [], invalid = new Set();
const clone = value => JSON.parse(JSON.stringify(value));
let pendingDxgiAction = null;
const dxgiAcknowledged = dxgi => { try { return localStorage.getItem('dxgiAck:'+dxgi.sha256) === '1'; } catch { return false; } };
const NO_TIMEOUT = new Set(['browse','save','import','export','importSboys','prepareSboys','diagnosticPackage','launch','apply','restore','grantWriteAccess','resumeTobii','updateInstall']);
const escape = value => String(value ?? '').replace(/[&<>"']/g,c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const empty = value => value.trim() || null;
const num = value => String(Number(Number(value).toFixed(4)));
/** A profile's own focus values in Pimax Play's Quick units, converted exactly like Pimax Quick (Pimax stores 4 decimals):
 *  Horizontal/Vertical FOV q is a focus q of the view (the percentage Pimax Play shows); Center Resolution c is a pixel density of c / 100
 *  (120% → 1.2); Peripheral Resolution r is a density of 0.01 + 0.98 × (r − 1) / 99 (15% → 0.1486). */
const fixed4 = v => Number(v.toFixed(4));
const PIMAX = {
  quickFromShare: s => s * 100, shareFromQuick: q => fixed4(q * .01),
  centerFromDensity: d => d * 100, densityFromCenter: c => fixed4(c / 100),
  peripheralPercent: d => 1 + (d - .01) / .98 * 99, peripheryFromPercent: v => fixed4(.01 + fixed4((v - 1) / 99) * .98)
};
/** Shown like Pimax Play shows its sliders: whole percent, rounded half up. Stored values change only when edited. */
const whole = v => Math.round(v);
const profileQuick = p => `${whole(PIMAX.quickFromShare(p.foveaWidth))}% × ${whole(PIMAX.quickFromShare(p.foveaHeight))}% · ${whole(PIMAX.centerFromDensity(p.quadFocusScale))}% · ${whole(PIMAX.peripheralPercent(p.peripheralScale))}%`;
function request(action, data = {}) {
  return new Promise((resolve,reject) => {
    if (!window.chrome?.webview) { reject(new Error('Open this interface in DCS Control to connect to its local services.')); return; }
    const id = String(++sequence);
    // A file dialog, a Windows prompt or a download waits for the pilot as long as it takes.
    const timer = NO_TIMEOUT.has(action) ? null : setTimeout(() => { pending.delete(id); reject(new Error('This took too long and was stopped. Press Check this PC on the Checks page, then try again.')); },120000);
    pending.set(id,{resolve,reject,timer}); window.chrome.webview.postMessage({id,action,data});
  });
}
window.chrome?.webview?.addEventListener('message',e => {
  const item = pending.get(e.data.id); if (!item) return; pending.delete(e.data.id); clearTimeout(item.timer);
  e.data.ok ? item.resolve(e.data.data) : item.reject(new Error(e.data.error));
});
const draft = extras => ({profile:clone(profile),dcs,options,...extras});
/** Each edit reaches the backend shortly after the last keystroke; its reply carries the checks and the dxgi.dll question
 *  for the draft as it is now, so the Checks page and Launch never work from an older draft's view. */
let changedTimer = null, changedGeneration = 0, changedInFlight = 0;
// keep: the draft did not change (the DCS-status poll); Review files and the checks stay valid. A pending edit wins.
let changedKeep = true;
function sendChangedSoon(keep = false) {
  changedKeep = (changedTimer === null || changedKeep) && keep;
  clearTimeout(changedTimer);
  changedTimer = setTimeout(() => {
    changedTimer = null;
    const keepNow = changedKeep;
    if (invalid.size) { request('changed').catch(showError); return; }
    const mine = changedGeneration; changedInFlight++;
    request('changed', {...draft(), keep: keepNow}).then(r => {
      // A command sent since (Import, Reset, Detect…) replied with newer checks: this reply is for an older draft.
      if (mine === changedGeneration) applyChanged(r);
    }).catch(showError).finally(() => changedInFlight--);
  }, 250);
}
function applyChanged(r) {
  if (!state || !r || !r.issues) return;
  state.issues = r.issues; state.foreignDxgi = r.foreignDxgi ?? null; if ('headsetRefresh' in r) state.headsetRefresh = r.headsetRefresh;
  renderEdits(); updateControls();
}
/** The draft on screen survives an app restart: saved (state root\draft.json) a moment after it last changed. */
function saveDraftSoon() {
  clearTimeout(draftTimer);
  draftTimer = setTimeout(() => {
    if (!profile || invalid.size) return;
    const d = draft(), json = JSON.stringify(d); if (json === savedDraftJson) return;
    savedDraftJson = json; request('saveDraft',d).catch(() => { savedDraftJson = null; });
  }, 700);
}
function showError(error) { $('status').textContent = error.message; $('statusTitle').textContent = 'Needs attention'; $('statusDot').classList.add('error'); $('toast').textContent = error.message; $('toast').hidden = false; clearTimeout(toastTimer); toastTimer = setTimeout(() => $('toast').hidden = true,9000); }
function setBusy(value) {
  busy = value; document.body.classList.toggle('busy',value);
  // The right-hand panel (route, features, setup) is outside #pages but must lock too: a feature switched during a
  // request would be silently replaced by the reply.
  document.querySelectorAll('#pages button, #pages input, #pages select, #pages textarea, .panel-top button, .panel-top input').forEach(el => { if (value) { el.dataset.wasDisabled = String(el.disabled); el.disabled = true; } else if (el.dataset.wasDisabled) { el.disabled = el.dataset.wasDisabled === 'true'; delete el.dataset.wasDisabled; } });
  ['importProfile','saveProfile'].forEach(id => $(id).disabled = value);
  // Controls whose state changed during the command (a reply, an update check) must not get the old one back.
  if (!value && state) { $('openSboys').disabled = !state.sboysReady; renderOriginals(); renderUpdates(); }
  updateWorkflow();
}
/** The short state of the draft, in the footer and on the Overview. */
const STATE_WORDS = {running:'DCS is running', ready:'Ready to fly', changes:'Changes apply at launch', invalid:'Check numeric values'};
const WORKING = {launch:'Applying the settings and starting DCS…', apply:'Applying the settings without starting DCS…', preview:'Listing the files Launch DCS writes…', restore:'Putting the original files back…'};
async function run(action, extras = {}) {
  if (invalid.size && ['preview','apply','launch','save','resetNeural','export','checkReadiness'].includes(action)) { const error = new Error('Correct the marked values first.'); showError(error); throw error; }
  // A second click while a command runs is ignored: Launch and Apply never run twice.
  if (busy) return; setBusy(true); $('statusTitle').textContent = 'Working'; $('status').textContent = WORKING[action] || 'Working…';
  if (action === 'preview' || action === 'apply' || action === 'launch') plan = null;
  // A pending edit goes with this command's draft; a "changed" already on its way is outdated by its reply.
  clearTimeout(changedTimer); changedTimer = null; changedGeneration++;
  try { const result = await request(action,draft(extras)); sync(result); if (action === 'preview') showPlan(); return result; }
  catch(error) { showError(error); throw error; }
  finally { setBusy(false); updateControls(); }
}
/** Review files: the read-only list of what Launch DCS writes for this draft. */
/** Shows the dxgi.dll question when it is still unanswered; the action then runs from the dialog. False: run it now. */
function askDxgiFirst(action) {
  const dxgi = state?.foreignDxgi; if (!dxgi || dxgiAcknowledged(dxgi)) return false;
  pendingDxgiAction = action;
  const name = dxgi.product || 'another program';
  $('dxgiText').textContent = `DCS's bin folder has a dxgi.dll from ${name}, and DLSS 5 needs that file name. Keep it working: DCS Control backs it up and loads it together with DLSS 5, so ${name} still works in DCS. Set it aside: it is backed up and stays off in DCS while this profile is applied.`;
  $('dxgiDialog').showModal(); return true;
}
/** askDxgiFirst after the checks caught up with the last edit (DLSS 5 just turned on brings the dxgi.dll question). */
async function askDxgiFresh(action, held = false) {
  if ((changedTimer || changedInFlight) && !invalid.size) {
    clearTimeout(changedTimer); changedTimer = null; const mine = ++changedGeneration;
    if (!held) setBusy(true);
    try { const r = await request('changed', draft()); if (mine === changedGeneration) applyChanged(r); } finally { if (!held) setBusy(false); }
  }
  return askDxgiFirst(action);
}
function showPlan() { navigate('diagnostics'); $('planCard').scrollIntoView({block:'start'}); }
function setFieldValue(f, value) {
  if (f.map) value = f.map.get(profile);
  if (f.type === 'hotkey') { f.input.value = value ?? ''; showHotkey(f); f.element.classList.remove('invalid'); return; }
  if (f.type === 'toggle') f.input.checked = value; else if (f.type === 'list') f.input.value = (value || []).join('\n'); else f.input.value = value ?? '';
  if (f.range) { f.range.value = value; paintRange(f.range); } f.element.classList.remove('invalid'); f.input.setAttribute('aria-invalid','false'); f.error && (f.error.hidden = true);
  syncSegment(f); if (f.listSummary) renderListSummary(f);
}
function sync(next) {
  state = next; if (next.version) $('appVersion').textContent = next.version.split('-')[0]; profile = clone(next.profile); dcs = next.dcs ?? ''; options = next.options ?? ''; plan = next.plan; invalid.clear();
  rememberProvider(profile);
  if (profile.frameGen !== 'Off') lastFrameGen = profile.frameGen;
  fields.forEach(f => setFieldValue(f, f.path === 'dcs' ? dcs : f.path === 'options' ? options : profile[f.path]));
  // The first reply is the draft already on disk (or the applied profile); later replies are saved when they differ.
  if (savedDraftJson === null) savedDraftJson = JSON.stringify(draft()); else saveDraftSoon();
  $('openSboys').disabled = !next.sboysReady;
  $('statusDot').classList.remove('error'); $('status').textContent = next.status + (next.foveaChanged ? ' ' + next.foveaChanged : '');
  renderDiagnostics(); renderOriginals(); updateControls(); renderPage(); boostChanged();
}
/** A provider forced by DLSS or the Sboys route is not a choice: the remembered provider stays as it is. */
function rememberProvider(p) { if (p.quadViews === 'PimaxNative' || (p.quadViews === 'QuadViewsFoveated' && nativeAllowed(p))) lastProvider = p.quadViews; }
/** The open page's panels that follow the draft (also after Import, Rescan and other replies). */
function renderPage() {
  if (page === 'engine') renderEngine();
  if (page === 'pupil') { renderPupilNeeds(); renderPupil(); }
  if (page === 'foveation') renderStabilityNeeds();
  if (page === 'visual') renderVisualNeeds();
  if (page === 'guide') renderGuide();
}
// ---- Features: what the checklist turns on, and what follows from it -------------------------------------------
// The <uses-rules> regions are pure twins of C# rules (VrProfile.Uses*, ProfileValidation.ResolveFeatures); the C# tests
// run them in Node over a table of profiles and compare ("UI rules match C#"). Keep them free of DOM and app state.
// <uses-rules>
/** Foveated Super Resolution runs only in stereo: with Quad Views it would redo DCS's DLSS of the focus views instead of
 *  DLSS 5 building on it, so Quad Views turns it off (resolveFeatures) and hides it. */
const foveatedSr = p => Boolean(p.foveatedDlss) && p.quadViews === 'None';
// The DLSS row stands for any DLSS processing: DLSS 5 neural rendering or, in stereo, Foveated Super Resolution on its own.
const featureState = p => ({engine:Boolean(p.engineOptimizations), boost:Boolean(p.cpuBoost), quad:p.quadViews !== 'None', dlss:Boolean(p.neuralRendering || foveatedSr(p)), framegen:p.frameGen !== 'Off'});
/** Pimax native Quad Views exists only on the Pimax route and cannot host the focus adapter DLSS 5 needs. */
const nativeAllowed = p => p.runtime !== 'SboysSteamVr' && !p.neuralRendering;
/** Cheeky without Quad Views (stereo): it clamps the focus and periphery ratios to at least 0.2. */
const stereoCheeky = p => Boolean(p.neuralRendering || p.foveatedDlss) && p.quadViews === 'None';
const CHEEKY_MIN = .2;
/** Settles combinations that cannot run, so a draft is always deployable as far as the features go. */
function resolveFeatures(p) {
  // Optimizations only keeps the VR settings for the next VR flight; they do not run (C# VrProfile.ForLaunch).
  // Foveated Super Resolution is for stereo only (same rule as ProfileValidation.ResolveFeatures).
  if (p.quadViews !== 'None') p.foveatedDlss = false;
  const cheeky = p.neuralRendering || p.foveatedDlss;
  // Pimax native Quad Views exists only in Pimax Play, and it cannot host the focus adapter that DLSS 5 needs.
  if (p.quadViews === 'PimaxNative' && (p.runtime === 'SboysSteamVr' || cheeky)) p.quadViews = 'QuadViewsFoveated';
  // With Quad Views, Cheeky always runs on the focus views through the adapter.
  p.quadFocusAdapter = Boolean(cheeky && p.quadViews === 'QuadViewsFoveated');
  return p;
}
// </uses-rules>
function setFeature(p, key, on) {
  if (key === 'quad') p.quadViews = on ? (p.quadViews !== 'None' ? p.quadViews : lastProvider) : 'None';
  // Checking it turns DLSS 5 on unless Foveated Super Resolution already runs; unchecking turns both off.
  if (key === 'dlss') { if (!on) { p.neuralRendering = false; p.foveatedDlss = false; } else if (!p.neuralRendering && !p.foveatedDlss) p.neuralRendering = true; }
  // Without an NVIDIA RTX card, frame generation starts as FidelityFX (it runs on any graphics card).
  if (key === 'framegen') p.frameGen = on ? (p.frameGen !== 'Off' ? p.frameGen : state?.gpu?.nvidiaRtx === false && lastFrameGen === 'Nvidia' ? 'FidelityFx' : lastFrameGen) : 'Off';
  if (key === 'boost') p.cpuBoost = on;
  if (key === 'engine') p.engineOptimizations = on;
  return resolveFeatures(p);
}
/** Profile name and id follow the route and the features, e.g. "Pimax · Quad Views + DLSS 5 + Frame generation + CPU Boost". */
function deriveIdentity(p) {
  if (p.desktop) return {id:'desktop-optimizations',name:'Optimizations only'};  const route = p.runtime === 'SboysSteamVr' ? 'Sboys' : 'Pimax', parts = [];
  if (p.quadViews === 'PimaxNative') parts.push(['nativeqv','Pimax Quad Views']); else if (p.quadViews !== 'None') parts.push(['qv','Quad Views']);
  if (p.neuralRendering) parts.push(['dlss5','DLSS 5']); else if (foveatedSr(p)) parts.push(['fdlss','Foveated Super Resolution']);
  if (p.frameGen !== 'Off') parts.push(['fg','Frame generation']);
  if (p.cpuBoost) parts.push(['boost','CPU Boost']);
  if (p.engineOptimizations) parts.push(['opt','Engine Optimizations']);
  return {id:route.toLowerCase()+'-'+(parts.length ? parts.map(x => x[0]).join('-') : 'original'), name:route+' · '+(parts.length ? parts.map(x => x[1]).join(' + ') : 'Original rendering')};
}
/** A copy of a profile with the route, focus movement and features chosen (used by the checklist and guided setup). */
function withFeatures(source, {route, features, gaze}) {
  const p = clone(source);
  if (route === 'Desktop') p.desktop = true;
  else if (route) { p.desktop = false; if (route !== p.runtime) { p.runtime = route; p.runtimeManifestPath = null; } }
  if (gaze) p.gaze = gaze;
  for (const [key] of FEATURES) setFeature(p, key, Boolean(features[key]));
  return Object.assign(p, deriveIdentity(p));
}
const pimaxSource = p => quad(p) && p.foveaSource === 'PimaxPlay';
/** The focus values that reach the provider: converted from Pimax Play when the profile follows it. */
function effectiveFovea(p) {
  const c = state?.pimax?.found ? state.pimax.converted : null;
  if (pimaxSource(p) && c) return {w:c.width,h:c.height,focus:c.focusScale,periphery:c.peripheralScale,blend:c.transition ? p.quadEdgeBlend : 0,pimax:true};
  // What is written: bundled Quad Views caps each axis at 0.9; stereo Cheeky uses at least 0.2.
  const cap = quad(p) ? .9 : 1, low = stereoCheeky(p) ? CHEEKY_MIN : 0, fit = v => Math.max(low, Math.min(cap, v));
  return {w:fit(p.foveaWidth),h:fit(p.foveaHeight),focus:p.quadFocusScale,periphery:Math.max(low,p.peripheralScale),blend:p.quadEdgeBlend,pimax:false};
}
/** The refresh rate in use: the headset's own when it follows the headset and one was read, otherwise the profile's. */
const refreshHz = p => p.refreshFromHeadset && state?.headsetRefresh?.hz ? state.headsetRefresh.hz : p.headsetRefreshHz;
const PRIORITY = {Normal:'Normal priority',AboveNormal:'Above normal',High:'High priority'};
function featureSummary(key, p) {
  if (key === 'engine') {
    if (!p.engineOptimizations) return 'Off · DCS as shipped';
    // The terrain prefetch fix counts as one more part when it skips repeats (Observe only counts calls).
    const n = ENGINE_PARTS.filter(k => p[k]).length + (p.boostPrefetch === 'Skip' ? 1 : 0);
    return `${n} of ${ENGINE_PARTS.length + 1} on · stays on outside the app`;
  }
  if (key === 'quad') {
    if (p.quadViews === 'None') return 'Off · normal stereo rendering';
    if (p.quadViews === 'PimaxNative') return 'Pimax native · ' + (state?.pimax?.found ? 'Pimax ' + state.pimax.short : 'uses Pimax Play settings');
    const e = effectiveFovea(p), gaze = p.gaze === 'EyeTracked' ? 'eye tracked' : 'fixed';
    if (p.quadViewsLayerDirectory) return 'Alternative provider · '+gaze;
    // In Pimax Play's units: "Pimax Quick 33% × 33% · 113% · 20%", or this profile's values in the same Quick units.
    if (pimaxSource(p)) return `Bundled · ${gaze} · ` + (state?.pimax?.found ? 'from Pimax Play\'s settings: ' + state.pimax.short : 'Pimax Play not found, values set here');
    return `Bundled · ${gaze} · set here · ${profileQuick(p)}`;
  }
  if (key === 'dlss') {
    if (!p.neuralRendering) return foveatedSr(p) ? 'Foveated Super Resolution on · DLSS 5 off' : 'Off · original image';
    const area = p.quadFocusAdapter ? (p.neuralFocusArea < 100 ? 'Central '+p.neuralFocusArea+'%' : 'Whole focus area') : 'Whole view';
    return [area, p.neuralStyle, foveatedSr(p) ? 'Foveated SR' : null, p.neuralRuntimePath || state?.savedRuntime ? null : 'runtime file missing'].filter(Boolean).join(' · ');
  }
  if (key === 'framegen') {
    if (p.frameGen === 'Off') return 'Off · rendered frames only';
    const factor = usesAdaptiveCadence(p) ? 'only when needed' : p.frameGenFactor === 0 ? 'Auto 2×/3×' : p.frameGenFactor+'×';
    return `${p.frameGen === 'Nvidia' ? 'NVIDIA' : 'FidelityFX'} · ${factor} · renders ${usesAdaptiveCadence(p) ? 'up to ' : ''}${Number((refreshHz(p)/capDivider(p)).toFixed(1))} FPS`;
  }
  // The flight helpers on the same page run with or without CPU Boost.
  const vr = !p.desktop;
  const helpers = [p.freeVram ? 'VRAM freed' : null, vr && p.pauseTobiiDesktop ? 'desktop Tobii paused' : null, vr && p.smallDcsWindow ? 'small DCS window' : null, vr && p.lowerMonitor ? `monitor ${p.flightDisplayWidth}×${p.flightDisplayHeight}` : null];
  if (!p.cpuBoost) return ['Off · Windows scheduling', ...helpers].filter(Boolean).join(' · ');
  const apps = p.boostMoveBackgroundApps ? (p.boostBackgroundApps || []).length : 0;
  return [PRIORITY[p.boostDcsPriority] || p.boostDcsPriority, apps ? apps+' apps moved' : null, ...helpers].filter(Boolean).join(' · ');
}
/** Short notes under a feature, only when they matter: [kind, text, field to reveal, link label]. */
function featureNotes(p) {
  const notes = {engine:[],boost:[],quad:[],dlss:[],framegen:[]}, cheeky = p.neuralRendering || p.foveatedDlss;
  // Only when the provider the user picked (Pimax native) was replaced.
  if (p.quadViews === 'QuadViewsFoveated' && lastProvider === 'PimaxNative' && !nativeAllowed(p))
    notes.quad.push(['info',cheeky ? "Bundled: Pimax native can't host DLSS 5." : 'Bundled: Pimax native is Pimax-route only.']);
  if (p.quadViews === 'QuadViewsFoveated' && cheeky && p.quadViewsLayerDirectory) notes.quad.push(['needs','DLSS 5 needs the bundled provider.','quadViewsLayerDirectory','Open']);
  if (pimaxSource(p) && state?.pimax && !state.pimax.found) notes.quad.push(['needs','Pimax Play values not found.','foveaSource','Open']);
  // The app keeps a verified copy of the runtime file, so it is asked for only when there is neither that copy nor a file of the profile's own.
  if (p.neuralRendering && !p.neuralRuntimePath && !state?.savedRuntime) notes.dlss.push(['needs','Runtime file missing.','neuralRuntimePath','Select file']);
  if ((p.cpuBoost && p.boostElevated) || (p.pauseTobiiDesktop && !p.desktop)) notes.boost.push(['info','Asks for administrator rights (UAC) at launch.']);
  return notes;
}
function noteHtml([kind,text,path,action]) { return `<div class="feature-note ${kind}"><span>${escape(text)}</span>${path ? `<button class="link-button" data-reveal="${path}"${action === 'Select file' ? ' data-browse-after="1"' : ''}>${escape(action)} →</button>` : ''}</div>`; }
function renderFeatures() {
  const on = featureState(profile), notes = featureNotes(profile);
  FEATURES.forEach(([key]) => {
    const item = $('feature-'+key); item.classList.toggle('on',on[key]);
    // On a monitor or with another headset only the optimizations are features here.
    item.hidden = Boolean(profile.desktop) && !['engine','boost'].includes(key);
    if (key === 'quad') $('featureGroup-vr').hidden = Boolean(profile.desktop);
    const box = $('featureCheck-'+key); box.checked = on[key]; box.disabled = busy;
    const summary = featureSummary(key,profile); $('featureSummary-'+key).textContent = summary; $('featureSummary-'+key).title = summary;
    $('featureNotes-'+key).innerHTML = notes[key].map(noteHtml).join('');
  });
  $('profileName').textContent = profile.name;
  // Undo changes: the draft differs from the applied profile and nothing locks it.
  $('resetApplied').hidden = !state?.appliedDraft || matchesApplied() || launchState() === 'running'; $('resetApplied').disabled = busy;
}
/** Back to the applied profile's settings, exactly as Launch DCS would use them. */
function resetToApplied() {
  if (busy || !state?.appliedDraft) return;
  profile = clone(state.appliedDraft); invalid.clear(); rememberProvider(profile);
  // The DCS install and options.lua the applied profile was written to.
  if (state.appliedDcs) dcs = state.appliedDcs; if (state.appliedOptions) options = state.appliedOptions;
  fields.forEach(f => { if (f.path === 'dcs') setFieldValue(f, dcs); else if (f.path === 'options') setFieldValue(f, options); });
  showProfile(); change(); boostChanged();
  $('status').textContent = 'Back to the applied settings.';
}
function change() {
  resolveFeatures(profile);
  // Pimax native comes back once nothing forces the bundled provider any more (DLSS off, Pimax route again).
  if (profile.quadViews === 'QuadViewsFoveated' && lastProvider === 'PimaxNative' && nativeAllowed(profile) && !profile.quadViewsLayerDirectory) profile.quadViews = 'PimaxNative';
  Object.assign(profile, deriveIdentity(profile));
  if (profile.frameGen !== 'Off') lastFrameGen = profile.frameGen;
  showProfile(FEATURE_FIELDS);
  plan = null; $('statusDot').classList.remove('error');
  if (state) { state.readiness = null; renderEdits(); }
  $('status').textContent = 'Settings changed. Launch DCS applies them; Review files lists what it will write.';
  sendChangedSoon(); updateControls(); saveDraftSoon(); renderPage();
}
/** "What happens at launch" depends only on the Boost settings and the route: refreshed for those edits, not every edit. */
function boostChanged() { if (page === 'boost') refreshBoostPlan(); }
// Fields the checklist and the route switch can change; other fields only change through their own input.
const FEATURE_FIELDS = ['quadViews','neuralRendering','foveatedDlss','frameGen','cpuBoost','engineOptimizations','pauseTobiiDesktop','runtimeManifestPath'];
/** Shows profile values in the fields (all, or only `paths`), never overwriting an entry the user is correcting. */
function showProfile(paths) {
  fields.forEach(f => { if (f.path === 'dcs' || f.path === 'options' || invalid.has(f.path) || (paths && !paths.includes(f.path))) return; setFieldValue(f, profile[f.path]); });
}
/** Help text as [lead, detail]: the lead shows, the detail waits behind a More button (a plain string is a lead alone). */
const helpParts = text => Array.isArray(text) ? [text[0], text[1] || ''] : [text, ''];
// More always stays on the line of the lead's last word, never alone on a line: a setting's lead ends with a word joiner
// (.setting-help>p::after), a card's last word and More share one nowrap span. nameMoreButtons() adds the title to its name.
const moreButton = id => `<button type="button" class="more-toggle" aria-expanded="false" aria-controls="${id}">More</button>`;
const gluedMore = (lead, id) => { const cut = lead.lastIndexOf(' ') + 1; return `${lead.slice(0,cut)}<span class="more-glue">${lead.slice(cut)}${moreButton(id)}</span>`; };
/** Every More button (settings, card subtitles, the static cards) is named with its own text plus the title it belongs to,
 *  so a screen reader says "More Smoothness buffer" rather than only "More". */
function nameMoreButtons() {
  document.querySelectorAll('.more-toggle:not([aria-labelledby])').forEach(b => {
    const id = b.getAttribute('aria-controls'), title = b.closest('.setting')?.querySelector('.setting-label') || b.closest('.card-heading')?.querySelector('h2'); if (!id || !title) return;
    if (!title.id) title.id = id+'-title'; b.id = id+'-toggle'; b.setAttribute('aria-labelledby',`${b.id} ${title.id}`);
  });
}
let cardMoreCount = 0;
function card(parent,title,subtitle) {
  const panel = document.createElement('div'); panel.className = 'card';
  const [lead, detail] = helpParts(subtitle), more = detail ? 'more-card-'+(++cardMoreCount) : '';
  panel.innerHTML = `<div class="card-heading"><div><h2>${title}</h2><p class="fine">${more ? gluedMore(lead,more) : lead}</p>${more ? `<p class="setting-more" id="${more}" hidden>${detail}</p>` : ''}</div></div>`; $(parent).append(panel); return panel;
}
/** A run of simple switches, drawn as one compact list inside a card or a fold. */
function toggleList(parent) { const list = document.createElement('div'); list.className = 'toggle-list'; parent.append(list); return list; }
/** What a card's feature still needs, at the top of the card, with buttons to the settings that give it (showNeeds). */
function needsNote(panel,id) {
  const box = document.createElement('div'); box.className = 'pimax-note warn card-note'; box.id = id; box.hidden = true;
  panel.querySelector(':scope > .card-heading').after(box); return box;
}
/** need: [text, [button id, setting path, label]...] or null; `waits` adds that the feature does not run until then. */
function showNeeds(id, need, waits) {
  const box = $(id); if (!box) return; box.hidden = !need;
  const html = need ? `<p>${escape(need[0])}${waits ? ' Until then it does not run.' : ''}</p><div class="inline-actions">${need.slice(1).map(([bid,path,label]) => `<button type="button" class="button secondary" id="${bid}" data-reveal="${path}">${escape(label)}</button>`).join('')}</div>` : '';
  if (box.dataset.html !== html) { box.dataset.html = html; box.innerHTML = html; }
}
/** What an eye-tracked feature (Pupil shift, Depth boost, Eye-focus blur) needs for a gaze, with the setting that gives it. */
const gazeNeed = (p, bid) => p.desktop ? null : p.quadViews === 'None' ? ['Needs Quad Views.',[bid,'quadViews','Turn on Quad Views']]
  : p.gaze !== 'EyeTracked' ? ['Needs an eye-tracked focus area.',[bid,'gaze','Set Focus movement']] : null;
/** A closed disclosure inside a card for settings that are real but rarely changed. Search and links open it. */
/** `value` (profile => text) shows the fold's current state in its closed summary (updateControls). */
const foldValues = [];
function disclosure(panel,title,subtitle,value) {
  const box = document.createElement('details'); box.className = 'advanced';
  box.innerHTML = `<summary>${title}<small>${subtitle}</small>${value ? '<span class="fold-value"></span>' : ''}</summary>`; panel.append(box);
  if (value) foldValues.push({box,value}); return box;
}
/** A field's value as its control shows it, for fold summaries. */
const shown = path => { const f = fields.find(x => x.path === path), v = f.map ? f.map.get(profile) : profile[path];
  return f.type === 'number' ? num(v)+(f.unit === 'ratio' ? '×' : f.unit === '°' ? '°' : f.unit ? ' '+f.unit : '') : f.type === 'select' ? f.input.selectedOptions[0]?.textContent.split(' · ')[0] : v ? 'on' : 'off'; };
/** A select with 2 to 4 short choices drawn as a segmented radio group. The select stays the field's input and its
 *  only source of truth; the buttons follow it (syncSegment). labels: {option value: short text}. */
function segmented(f, labels = {}) {
  const select = f.input, path = f.path, label = f.element.querySelector(`label[for="${select.id}"]`);
  select.classList.add('seg-native'); select.tabIndex = -1; select.setAttribute('aria-hidden','true');
  label.id = 'label-'+path; label.removeAttribute('for'); f.element.classList.add('has-seg');
  const group = document.createElement('div'); group.className = 'segmented'; group.id = 'seg-'+path; group.setAttribute('role','radiogroup');
  // A choice that cannot be picked now says why in a line under the group (a disabled button cannot take focus for its tooltip).
  const note = document.createElement('span'); note.className = 'seg-note'; note.id = 'segnote-'+path; note.hidden = true;
  group.setAttribute('aria-labelledby','label-'+path); group.setAttribute('aria-describedby',((select.getAttribute('aria-describedby') || '')+' '+note.id).trim());
  const buttons = [...select.options].map(o => {
    const b = document.createElement('button'); b.type = 'button'; b.setAttribute('role','radio'); b.dataset.value = o.value; b.title = o.textContent;
    b.textContent = labels[o.value] ?? o.textContent.split(/ · | \(/)[0];
    // The default dot, unless the label already says Default.
    if (/recommended|default/i.test(o.textContent) && !/^default$/i.test(b.textContent)) b.classList.add('is-default'); return b;
  });
  group.append(...buttons); select.after(group); group.after(note); f.segments = {group,buttons,note};
  const choose = b => { if (!b || b.disabled) return; if (select.value !== b.dataset.value) { select.value = b.dataset.value; select.dispatchEvent(new Event('change',{bubbles:true})); } syncSegment(f); };
  group.addEventListener('click',e => choose(e.target.closest('button')));
  group.addEventListener('keydown',e => {
    const enabled = buttons.filter(b => !b.disabled); if (!enabled.length) return;
    const at = Math.max(0, enabled.indexOf(e.target.closest('button'))), n = enabled.length;
    const next = {ArrowLeft:at-1,ArrowUp:at-1,ArrowRight:at+1,ArrowDown:at+1,Home:0,End:n-1}[e.key]; if (next === undefined) return;
    e.preventDefault(); const target = enabled[(next+n)%n]; choose(target); target.focus();
  });
  syncSegment(f); return f;
}
function syncSegment(f) {
  if (!f.segments) return; const {group,buttons} = f.segments, select = f.input;
  buttons.forEach((b,i) => { const o = select.options[i]; b.setAttribute('aria-checked',String(o.value === select.value)); b.disabled = select.disabled || o.disabled; b.title = o.textContent; });
  const stop = buttons.find(b => b.getAttribute('aria-checked') === 'true' && !b.disabled) || buttons.find(b => !b.disabled);
  buttons.forEach(b => b.tabIndex = b === stop ? 0 : -1); group.setAttribute('aria-disabled',String(select.disabled));
  // While the setting itself can change, a choice that cannot be picked shows the reason its option text gives ("· not with DLSS 5").
  const why = select.disabled ? [] : buttons.filter((b,i) => b.disabled && select.options[i].textContent.includes(' · ')).map(b => `${b.textContent}: ${b.title.split(' · ').slice(1).join(' · ')}.`);
  const text = why.join(' '); if (f.segments.note.textContent !== text) f.segments.note.textContent = text; f.segments.note.hidden = !text;
}
/** A list field's one-line summary ("None", or "N programs: a, b, c +K") above its folded textarea. */
function renderListSummary(f) {
  const items = f.input.value.split(/[\n,;]/).map(s => s.trim()).filter(Boolean), n = items.length;
  f.listSummary.querySelector('.list-summary-text').textContent = !n ? 'None' : `${n} program${n === 1 ? '' : 's'}: ${items.slice(0,3).join(', ')}${n > 3 ? ' +'+(n-3) : ''}`;
}
function openList(f, open) {
  f.input.hidden = !open; f.listSummary.setAttribute('aria-expanded',String(open)); f.listSummary.querySelector('.list-summary-action').textContent = open ? 'Done' : 'Edit';
  if (open) f.input.focus({preventScroll:true});
}
/** `map` shows a profile value in other units ({get(profile), set(profile,value)}); the profile keeps its own value until edited.
 *  `why` says what makes a greyed-out setting editable: a text, [text, path of a setting to open], or a function of the profile. */
function field(panel,path,title,description,type,values,condition,unit='',hide=null,map=null,why=null) {
  const element = document.createElement('div'); element.className = 'setting'; element.id = 'setting-'+path;
  const id = 'input-'+path; let inputMarkup;
  // The lead stays visible; the detail is in the page (and read by screen readers) behind More.
  const [lead, detail] = helpParts(description), described = 'help-'+path+(detail ? ' more-'+path : '');
  const help = `<div class="setting-help"><p id="help-${path}">${lead}</p>${detail ? moreButton('more-'+path) : ''}</div>${detail ? `<p class="setting-more" id="more-${path}" hidden>${detail}</p>` : ''}<span class="why" id="why-${path}" hidden></span>`;
  // A switch's label holds only its title: the More button cannot sit inside a label.
  if (type === 'toggle') inputMarkup = `<div class="switch-row"><div class="switch-text"><label class="setting-label" id="label-${path}" for="${id}">${title}</label>${help}</div><input id="${id}" class="switch" type="checkbox" role="switch" aria-describedby="${described}"></div>`;
  else {
    inputMarkup = `<label class="setting-label" id="label-${path}" for="${id}">${title}</label>${help}`;
    if (type === 'select') inputMarkup += `<select id="${id}" aria-describedby="${described}">${values.map(([v,t]) => `<option value="${v}">${t}</option>`).join('')}</select>`;
    if (type === 'number') inputMarkup += `<div class="setting-number"><input id="range-${path}" type="range" min="${values[0]}" max="${values[1]}" step="${values[2]}" aria-label="${title} slider"><input id="${id}" class="number-input" type="text" inputmode="decimal" autocomplete="off" spellcheck="false" min="${values[0]}" max="${values[1]}" aria-describedby="${described} error-${path}"><span class="number-unit">${unit === 'ratio' ? '×' : unit}</span></div><span id="error-${path}" class="input-error" hidden></span>`;
    if (type === 'path') inputMarkup += `<div class="path-row"><input id="${id}" type="text" spellcheck="false" aria-describedby="${described}"><button class="button secondary" id="browse-${path}">Browse</button></div>`;
    // A list starts folded into a one-line summary; Edit opens the textarea.
    if (type === 'list') inputMarkup += `<button type="button" class="list-summary" id="listsum-${path}" aria-expanded="false" aria-controls="${id}" aria-labelledby="label-${path} listsum-${path}"><span class="list-summary-text"></span><span class="list-summary-action">Edit</span></button><textarea id="${id}" class="list-input" rows="5" spellcheck="false" aria-describedby="${described}" hidden></textarea>`;
    if (type === 'hotkey') inputMarkup += `<div class="hotkey-row"><div class="hotkey-line"><button type="button" id="${id}" class="hotkey-capture" aria-describedby="${described} keynote-${path}"></button><button type="button" id="default-${path}" class="button quiet hotkey-default" hidden>Default</button></div><span id="keynote-${path}" class="hotkey-note" role="status"></span></div>`;
  }
  element.innerHTML = inputMarkup; panel.append(element); const input = $(id), range = $('range-'+path), error = $('error-'+path);
  input.setAttribute('aria-describedby',((input.getAttribute('aria-describedby') || '')+' why-'+path).trim());
  const f = {path,prop:map?.prop ?? path,title,description:lead,detail,type,values,condition,unit,element,input,range,error,hide,map,why,whyElement:$('why-'+path),page:panel.closest('.page').id.slice(5)}; fields.push(f);
  if (type === 'hotkey') captureHotkey(f);
  if (type === 'list') { element.classList.add('has-list'); f.listSummary = $('listsum-'+path); f.listSummary.addEventListener('click',() => openList(f, f.input.hidden)); }
  input.addEventListener(type === 'toggle' || type === 'select' || type === 'hotkey' ? 'change' : 'input', () => {
    let value = type === 'toggle' ? input.checked : input.value;
    if (type === 'number') {
      // "0,5" (a decimal comma, typed on many European keyboards and number pads) reads as 0.5.
      // A whole-number step (the slider's) takes whole numbers only: the profile stores some of these as integers.
      const n = Number(String(value).trim().replace(',', '.')), whole = Number.isInteger(values[2]), valid = String(value).trim() !== '' && Number.isFinite(n) && n >= values[0] && n <= values[1] && (!whole || Number.isInteger(n));
      element.classList.toggle('invalid',!valid); input.setAttribute('aria-invalid',String(!valid)); error.hidden = valid;
      if (!valid) { invalid.add(path); error.textContent = `Enter a ${whole ? 'whole number' : 'value'} from ${values[0]} to ${values[1]}.`; change(); return; }
      invalid.delete(path); value = n; range.value = n; paintRange(range);
    } else if (type === 'select' && typeof values[0][0] === 'number') value = Number(value);
    else if (type === 'list') value = value.split(/[\n,;]/).map(s => s.trim()).filter(Boolean);
    if (path === 'dcs') dcs = value; else if (path === 'options') options = value; else if (map) map.set(profile,value); else profile[path] = type === 'path' ? empty(value) : value;
    syncSegment(f); if (f.listSummary) renderListSummary(f);
    // Another field showing the same profile value (in other units) follows the edit.
    fields.forEach(o => { if (o !== f && o.prop === f.prop && !invalid.has(o.path)) setFieldValue(o, profile[o.path]); });
    // Only a provider the user selects is remembered (for turning Quad Views back on, or after DLSS is turned off).
    if (path === 'quadViews' && value !== 'None') lastProvider = value;
    change(); if (f.page === 'boost') boostChanged();
  });
  if (range) range.addEventListener('input',() => { input.value = range.value; input.dispatchEvent(new Event('input',{bubbles:true})); });
  if (type === 'path') $('browse-'+path).addEventListener('click',async () => {
    if (busy) return; setBusy(true);
    try {
      const result = await request('browse',{target:path}); if (!result.path) return;
      // The DLSS 5 runtime is verified and kept as the app's copy; the profile then uses that copy (an empty path).
      if (path === 'neuralRuntimePath') { const saved = await saveRuntime(result.path); input.value = ''; input.dispatchEvent(new Event('input',{bubbles:true})); $('status').textContent = saved.message; }
      else { input.value = result.path; input.dispatchEvent(new Event('input',{bubbles:true})); }
    }
    catch(error) { showError(error); } finally { setBusy(false); updateControls(); }
  });
  return f;
}
// ---- In-flight keys: recorded from the keyboard as "virtual-key:modifiers" (Ctrl 1, Alt 2, Shift 4) -----------------
// Profiles of earlier versions hold one of the labels the old menu offered; they keep working (NeuralHotkeys.cs).
// <uses-rules>
const LEGACY_KEYS = {'Off':[0,0],'Ctrl+Shift+F12':[123,5],'Alt+Shift+F12':[123,6],'Ctrl+Alt+Shift+F10':[121,7],'Ctrl+Alt+Shift+F12':[123,7],'Ctrl+Alt+Shift+Home':[36,7],'Ctrl+Alt+Shift+End':[35,7],'Ctrl+Alt+Shift+Insert':[45,7]};
const KEY_NAMES = {8:'Backspace',9:'Tab',13:'Enter',19:'Pause',20:'Caps Lock',27:'Esc',32:'Space',33:'Page Up',34:'Page Down',35:'End',36:'Home',37:'Left',38:'Up',39:'Right',40:'Down',44:'Print Screen',45:'Insert',46:'Delete',106:'Num *',107:'Num +',109:'Num -',110:'Num .',111:'Num /',144:'Num Lock',145:'Scroll Lock',186:';',187:'=',188:',',189:'-',190:'.',191:'/',192:'`',219:'[',220:'\\',221:']',222:"'",226:'\\ (102nd key)'};
function parseKey(value) {
  if (value == null) return null;
  const legacy = Object.keys(LEGACY_KEYS).find(k => k.toLowerCase() === String(value).trim().toLowerCase()); if (legacy) return LEGACY_KEYS[legacy];
  const m = /^(\d{1,3}):([0-7])$/.exec(String(value).trim()); if (!m) return null;
  const vk = Number(m[1]), mods = Number(m[2]); return vk <= 254 && (vk || !mods) ? [vk,mods] : null;
}
// </uses-rules>
const keyName = vk => vk >= 0x70 && vk <= 0x87 ? 'F'+(vk-0x6F) : (vk >= 48 && vk <= 57) || (vk >= 65 && vk <= 90) ? String.fromCharCode(vk) : vk >= 96 && vk <= 105 ? 'Num '+(vk-96) : KEY_NAMES[vk] || 'Key '+vk;
const modifierNames = mods => [mods & 1 ? 'Ctrl' : null, mods & 2 ? 'Alt' : null, mods & 4 ? 'Shift' : null].filter(Boolean);
const keyLabel = k => !k || !k[0] ? 'Off' : [...modifierNames(k[1]), keyName(k[0])].join('+');
const keyCode = k => k[0]+':'+k[1];
const worksAlone = vk => (vk >= 0x70 && vk <= 0x87) || [19,145,45,36,35,33,34].includes(vk);
/** Why a key cannot be used, as NeuralHotkeys.Problem says it; null when it can. */
function keyProblem([vk,mods]) {
  if (!vk) return null;
  if ([16,17,18,20,91,92,93].includes(vk) || (vk >= 160 && vk <= 165)) return 'Hold Ctrl, Alt or Shift together with another key.';
  if (vk === 229) return 'This key has no usable key code. Choose another key.';
  if (['115:2','9:2','27:1','27:2','27:5','32:2','46:3'].includes(vk+':'+mods)) return keyLabel([vk,mods])+' is used by Windows.';
  if (!mods && !worksAlone(vk)) return keyLabel([vk,mods])+' needs Ctrl, Alt or Shift. F-keys, Pause, Scroll Lock, Insert, Home, End, Page Up and Page Down also work alone.';
  return null;
}
const HOTKEYS = {
  neuralToggleKey:{others:['diagnosticOverlayKey','engineToggleKey','pupilShiftToggleKey','depthBoostToggleKey','farDepthToggleKey','colorModeKey'],used:p => p.neuralRendering,name:'DLSS 5',fallback:'Ctrl+Shift+F12'},
  diagnosticOverlayKey:{others:['neuralToggleKey','engineToggleKey','pupilShiftToggleKey','depthBoostToggleKey','farDepthToggleKey','colorModeKey'],used:p => !p.desktop && (p.frameGen !== 'Off' || p.headsetPanel),name:'headset panel',fallback:'Alt+Shift+F12'},
  engineToggleKey:{others:['neuralToggleKey','diagnosticOverlayKey','pupilShiftToggleKey','depthBoostToggleKey','farDepthToggleKey','colorModeKey'],used:p => p.engineOptimizations,name:'Engine Optimizations',fallback:'122:6'},
  pupilShiftToggleKey:{others:['neuralToggleKey','diagnosticOverlayKey','engineToggleKey','depthBoostToggleKey','farDepthToggleKey','colorModeKey'],used:p => usesPupilShift(p),name:'Pupil shift',fallback:'121:5'},
  depthBoostToggleKey:{others:['neuralToggleKey','diagnosticOverlayKey','engineToggleKey','pupilShiftToggleKey','farDepthToggleKey','colorModeKey'],used:p => usesDepthBoost(p),name:'Depth boost',fallback:'121:6'},
  farDepthToggleKey:{others:['neuralToggleKey','diagnosticOverlayKey','engineToggleKey','pupilShiftToggleKey','depthBoostToggleKey','colorModeKey'],used:p => !p.desktop && p.farDepth && quad(p) && p.engineOptimizations,name:'Far depth',fallback:'119:6'},
  colorModeKey:{others:['neuralToggleKey','diagnosticOverlayKey','engineToggleKey','pupilShiftToggleKey','depthBoostToggleKey','farDepthToggleKey'],used:p => !p.desktop,name:'Color mode',fallback:'Off'}
};
/** Ctrl+Alt+F8 to F12 and Ctrl+Alt+Page Up/Down, with or without Shift, belong to the engine optimizations module itself. */
const engineReserved = ([vk,mods]) => (mods & 3) === 3 && ((vk >= 0x77 && vk <= 0x7B) || vk === 33 || vk === 34);
const isDcsDefault = k => Boolean(k && k[0] && state?.dcsDefaultKeys?.includes(keyCode(k)));
/** The recorded key, and a note when DCS binds it by default (it still works; DCS reacts too). */
function showHotkey(f, message, kind) {
  if (!f.input.classList.contains('capturing')) f.input.innerHTML = `<kbd>${escape(keyLabel(parseKey(f.input.value) ?? parseKey(HOTKEYS[f.path].fallback)))}</kbd>`;
  const k = parseKey(f.input.value), note = $('keynote-'+f.path), fallback = parseKey(HOTKEYS[f.path].fallback), reset = $('default-'+f.path);
  // Default shows only while the key differs from its default (an earlier version's spelling of the same key does not).
  if (reset) { reset.hidden = !k || keyCode(k) === keyCode(fallback) || f.input.classList.contains('capturing'); reset.disabled = f.input.disabled; reset.title = 'Back to '+keyLabel(fallback); }
  if (!message && isDcsDefault(k)) { message = `DCS also uses ${keyLabel(k)} by default.`; kind = 'warn'; }
  note.textContent = message || ''; note.className = 'hotkey-note' + (kind ? ' '+kind : '');
}
/** Click, then press the combination: Esc cancels, Backspace clears (Off). Keys used by Windows, keys without a
 *  modifier (other than F-keys and the like) and the other in-flight key are refused; a DCS default is accepted with a note. */
function captureHotkey(f) {
  const button = f.input, info = HOTKEYS[f.path];
  const stop = message => { button.classList.remove('capturing'); button.removeAttribute('aria-pressed'); window.removeEventListener('keydown',onKey,true); window.removeEventListener('keyup',onUp,true); showHotkey(f,message,message ? 'error' : null); };
  const prompt = mods => { button.innerHTML = `<kbd>${escape(mods ? modifierNames(mods).join('+')+'+…' : 'Press keys…')}</kbd>`; };
  function onUp(e) { e.preventDefault(); e.stopPropagation(); prompt((e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0)); }
  function onKey(e) {
    e.preventDefault(); e.stopPropagation();
    const mods = (e.ctrlKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0), vk = e.keyCode;
    if ([16,17,18].includes(vk)) { prompt(mods); return; }
    // The native parsers read virtual keys 1-254 with Ctrl, Alt and Shift only: the Windows key, IME and media keys
    // (no key code, 229 or 255) cannot be recorded.
    const refused = e.metaKey || vk === 91 || vk === 92 ? 'The Windows key cannot be part of an in-flight key.'
      : !vk || vk >= 255 || vk === 229 ? 'This key has no usable key code. Choose another key.' : null;
    if (refused) { showHotkey(f,refused,'error'); prompt(0); return; }
    if (vk === 27 && !mods) { stop(); return; }
    let next = [vk,mods];
    if ((vk === 8 || vk === 46) && !mods) next = [0,0];
    const problem = keyProblem(next);
    if (problem) { showHotkey(f,problem,'error'); prompt(0); return; }
    const taken = info.others.find(o => HOTKEYS[o].used(profile) && (k => k && keyCode(k) === keyCode(next))(parseKey(profile[o] ?? HOTKEYS[o].fallback)));
    if (next[0] && taken) { showHotkey(f,`${keyLabel(next)} is already the ${HOTKEYS[taken].name} key.`,'error'); prompt(0); return; }
    if (next[0] && engineReserved(next)) { showHotkey(f,`${keyLabel(next)} is reserved by Engine Optimizations.`,'error'); prompt(0); return; }
    const current = parseKey(button.value);
    stop();
    // The same combination keeps the profile's own spelling (an earlier version's label stays as it is).
    if (!current || keyCode(current) !== keyCode(next)) { button.value = next[0] ? keyCode(next) : 'Off'; button.dispatchEvent(new Event('change',{bubbles:true})); }
    showHotkey(f);
  }
  $('default-'+f.path).addEventListener('click',() => {
    if (busy || button.disabled) return;
    const next = parseKey(info.fallback), taken = next[0] && info.others.find(o => HOTKEYS[o].used(profile) && (k => k && keyCode(k) === keyCode(next))(parseKey(profile[o] ?? HOTKEYS[o].fallback)));
    if (taken) { showHotkey(f,`${keyLabel(next)} is already the ${HOTKEYS[taken].name} key.`,'error'); return; }
    button.value = info.fallback; button.dispatchEvent(new Event('change',{bubbles:true})); showHotkey(f);
  });
  button.addEventListener('click',() => {
    if (busy || button.disabled) return;
    if (button.classList.contains('capturing')) { stop(); return; }
    button.classList.add('capturing'); button.setAttribute('aria-pressed','true'); prompt(0); $('default-'+f.path).hidden = true;
    $('keynote-'+f.path).textContent = 'Esc cancels · Backspace clears'; $('keynote-'+f.path).className = 'hotkey-note';
    window.addEventListener('keydown',onKey,true); window.addEventListener('keyup',onUp,true);
  });
  button.addEventListener('blur',() => { if (button.classList.contains('capturing')) stop(); });
}
// <uses-rules>
/** quad: the bundled Quad Views (VrProfile.UsesBundledQuadViews); a blank layer folder is no folder. */
const nr = p => p.neuralRendering, flow = p => p.frameGen === 'Nvidia', quad = p => p.quadViews === 'QuadViewsFoveated' && !String(p.quadViewsLayerDirectory ?? '').trim();
/** Frame generation only when needed (VrProfile.UsesAdaptiveCadence): Auto only; a pinned 2× or 3× always generates. */
const usesAdaptiveCadence = p => Boolean(p.frameGen !== 'Off' && !p.desktop && p.frameGenAdaptiveCadence && p.frameGenFactor === 0);
/** The refresh rate divided by this is what DCS should render (FramePacing.CapDivider). */
const capDivider = p => usesAdaptiveCadence(p) ? 1 : p.frameGen === 'Off' ? 1 : p.frameGenFactor === 3 ? 3 : 2;
// </uses-rules>
const coverageOwned = p => quad(p) || stereoCheeky(p);
const boost = p => p.cpuBoost;
// Monitor modes as "1920x1080@60" in the select; the profile keeps width, height and refresh.
const profileDefaults = {flightDisplayWidth:1920, flightDisplayHeight:1080, flightDisplayRefresh:60};
const displayModeKey = p => `${p.flightDisplayWidth}x${p.flightDisplayHeight}@${p.flightDisplayRefresh}`;
const displayModeLabel = p => `${p.flightDisplayWidth}×${p.flightDisplayHeight} at ${p.flightDisplayRefresh} Hz`;
/** The monitor's own modes in the flight mode list, plus the profile's mode when the monitor does not report it. */
function fillDisplayModes(monitor) {
  const f = fields.find(x => x.path === 'flightDisplayMode'); if (!f || !profile) return;
  const modes = (monitor?.modes || []).map(m => ({flightDisplayWidth:m.width, flightDisplayHeight:m.height, flightDisplayRefresh:m.refresh}));
  const own = displayModeKey(profile), current = monitor ? `${monitor.current.width}x${monitor.current.height}@${monitor.current.refresh}` : null;
  const options = modes.map(m => [displayModeKey(m), displayModeLabel(m) + (displayModeKey(m) === current ? ' · current' : '')]);
  if (!options.some(([k]) => k === own)) options.unshift([own, displayModeLabel(profile) + (monitor ? ' · not reported by this monitor' : '')]);
  f.input.innerHTML = options.map(([v,t]) => `<option value="${v}">${escape(t)}</option>`).join('');
  setFieldValue(f, null);
}
function buildFields() {
  // Quad Views page: provider and focus movement (with what follows the eyes) first, then the focus area's size and shape;
  // its sharpness has its own card under the coverage preview.
  let c = card('foveationControls','Foveated rendering','Sharp where you look, lower resolution around it.');
  segmented(field(c,'quadViews','Quad Views provider',['Bundled works with every DCS Control feature.','Pimax native uses Pimax Play\'s own Quad Views: Pimax route only, not with DLSS 5.'],'select',[['None','Off · normal stereo'],['QuadViewsFoveated','Bundled Quad Views (recommended)'],['PimaxNative','Pimax native Quad Views']]),{None:'Off',QuadViewsFoveated:'Bundled',PimaxNative:'Pimax native'});
  segmented(field(c,'gaze','Focus movement','Eye tracked follows your eyes; Fixed stays in the center.','select',[['EyeTracked','Eye tracked'],['Fixed','Fixed (centered)']],p => p.quadViews !== 'None'),{EyeTracked:'Eye tracked',Fixed:'Fixed'});
  const tracked = p => quad(p) && p.gaze === 'EyeTracked';
  let toggles = toggleList(c);
  field(toggles,'quadSaccadeLead','Focus ahead of fast eye movements',['Lands you in full detail after a fast eye jump.','When your eyes jump, the focus area reaches ahead along the jump for a moment, instead of waiting a frame or two (most noticeable with frame generation). No GPU cost: for that moment the focus area is a little less sharp.'],'toggle',null,tracked);
  field(toggles,'quadGazeStabilize','Steady focus while looking',['Stops the focus edge shimmering while your eyes rest.','Movements under half a degree are ignored, so eye-tracker noise does not move the focus area. Real eye movements are followed at once.'],'toggle',null,tracked);
  field(toggles,'quadTurbo','Turbo mode',['Can help when the CPU limits FPS.','DCS starts the next frame while the headset still shows the previous one.'],'toggle',null,quad);
  let more = disclosure(c,'Alternative provider','Only to test another Quad Views build.',p => p.quadViews !== 'QuadViewsFoveated' ? 'Not used' : p.quadViewsLayerDirectory ? p.quadViewsLayerDirectory.split(/[\\/]/).filter(Boolean).pop() : 'Bundled');
  field(more,'quadViewsLayerDirectory','Alternative Quad Views provider','Folder of another Quad Views build. Leave empty for the bundled one, which DLSS 5 needs.','path',null,p => p.quadViews === 'QuadViewsFoveated');
  c = card('focusControls','Focus area','Size and detail of the sharp area, in Pimax Play\'s units.');
  segmented(field(c,'foveaSource','Focus area values',['Read from Pimax Play at every launch, or set here.','From Pimax Play: the Quad View values you set there, read again at every launch. Nothing is written to Pimax Play. Set here: choose them below, in the same units.'],'select',[['PimaxPlay','From Pimax Play (recommended)'],['Profile','Set here']],quad),{PimaxPlay:'From Pimax Play',Profile:'Set here'});
  const pimaxBox = document.createElement('div'); pimaxBox.id = 'pimaxFovea'; pimaxBox.className = 'pimax-fovea'; pimaxBox.hidden = true; c.append(pimaxBox);
  // This profile's own focus with bundled Quad Views, edited like Pimax Play's Quick sliders and converted exactly as Pimax converts them.
  const ownFocusHidden = p => p.quadViews !== 'QuadViewsFoveated' || pimaxSource(p);
  field(c,'quadFocusScale','Center resolution','Detail inside the focus area. Above 100% is sharper and costs more GPU.','number',[50,200,1],quad,'%',ownFocusHidden,{prop:'quadFocusScale',get:p => whole(PIMAX.centerFromDensity(p.quadFocusScale)),set:(p,v) => p.quadFocusScale = PIMAX.densityFromCenter(v)});
  field(c,'pimaxPeripheral','Peripheral resolution',['Resolution around the focus area. Lower saves GPU.','As in Pimax Play. Bundled Quad Views needs at least 16%.'],'number',[16,100,1],quad,'%',ownFocusHidden,{prop:'peripheralScale',get:p => whole(PIMAX.peripheralPercent(p.peripheralScale)),set:(p,v) => p.peripheralScale = PIMAX.peripheryFromPercent(v)});
  field(c,'pimaxHorizontalFov','Horizontal FOV',['The focus area\'s share of the view\'s width.','As in Pimax Play\'s Quick mode: 25% is a focus area a quarter of the view wide.'],'number',[10,90,1],quad,'%',ownFocusHidden,{prop:'foveaWidth',get:p => whole(PIMAX.quickFromShare(p.foveaWidth)),set:(p,v) => p.foveaWidth = PIMAX.shareFromQuick(v)});
  field(c,'pimaxVerticalFov','Vertical FOV',['The focus area\'s share of the view\'s height.','As in Pimax Play\'s Quick mode, top to bottom.'],'number',[10,90,1],quad,'%',ownFocusHidden,{prop:'foveaHeight',get:p => whole(PIMAX.quickFromShare(p.foveaHeight)),set:(p,v) => p.foveaHeight = PIMAX.shareFromQuick(v)});
  field(c,'quadEdgeBlend','Focus edge blending',['How softly the focus area fades into the periphery.','Wider is softer but leaves a little less area in full detail. With values from Pimax Play and its Transition Mode Off, the edge is hard (0).'],'number',[0,.5,.01],quad,'ratio');
  field(c,'quadRoundFocus','Round focus area',['On: a circle. Off: a rounded rectangle, with more detail in the corners.','Either way its edge fades out over Focus edge blending. A central DLSS 5 area follows the same shape.'],'toggle',null,quad);
  c = card('sharpnessControls','Sharpness','Detail in and around the focus area. Almost no GPU cost.');
  field(c,'quadSharpening','Focus sharpening','Extra sharpening inside the focus area. 0 is off.','number',[0,1,.01],quad,'ratio');
  field(c,'quadSharpenTaper','Sharpening fades at the edge',['Fades the sharpening toward the focus edge, so the edge is harder to see.','Detail steps down gradually. Almost no GPU cost.'],'toggle',null,quad);
  field(c,'quadPeripheryContrast','Periphery contrast',['Makes the periphery look less blurry. 0 is off.','Brings back the local contrast the periphery loses at its lower resolution. Almost no GPU cost. Raise it slowly: too much makes the periphery shimmer (Periphery stability calms that).'],'number',[0,1,.05],quad,'ratio');
  // DLSS page: DLSS 5 (on/off, runtime file, the dxgi switch) and its image, then DCS's DLSS presets, then Foveated Super
  // Resolution (stereo only) with the focus area it uses without Quad Views.
  c = card('dlssControls','DLSS 5','NVIDIA\'s neural rendering. Its runtime file is not included: you provide it.');
  field(c,'neuralRendering','Use DLSS 5','Runs DLSS 5 on the image. With Quad Views, only on the focus area.','toggle');
  field(c,'neuralRuntimePath','Runtime file',['NVIDIA\'s nvngx_dlssnr.dll, 310.8 or 310.9. Select it once.','310.9 is NVIDIA\'s newer, faster model; 310.8 is the one tested most. On RTX 20, 30 and 40 cards, NVIDIA\'s file patched for them works too. DCS Control keeps a verified copy and uses it while this field is empty.'],'path',null,nr);
  const savedBox = document.createElement('div'); savedBox.id = 'savedRuntime'; savedBox.className = 'saved-runtime'; savedBox.hidden = true; $('setting-neuralRuntimePath').append(savedBox);
  field(c,'keepOtherDxgi','Keep another program\'s dxgi.dll working',['For ReShade or an overlay with its own dxgi.dll in DCS.','On: it keeps working with DLSS 5. Off: it is set aside while you fly. Back to stock DCS puts it back.'],'toggle',null,p => Boolean(p.neuralRendering || p.foveatedDlss));
  // Area first (the biggest GPU cost), then resolution, then the look.
  c = card('dlssProcessing','DLSS 5 image','Area and working scale set the GPU cost; intensity and style the look.');
  field(c,'neuralFocusArea','DLSS 5 area',['How much of the focus area DLSS 5 processes.','Around it the image is normal DLSS. Times are GPU time for both eyes, measured on an RTX 5090 (1764×2480 per eye, full working scale).'],'select',[[100,'Whole focus area · 10.8 ms · default'],[90,'Central 90% · about 9.3 ms (estimated)'],[80,'Central 80% · 7.9 ms'],[70,'Central 70% · 6.4 ms'],[50,'Central 50% · 4.8 ms']],p => nr(p) && p.quadViews === 'QuadViewsFoveated' && p.quadFocusAdapter);
  field(c,'quadNeuralEdgeFade','DLSS 5 fades at the focus edge',['The DLSS 5 look fades toward the focus edge, so it does not stand out. No GPU cost.','Tone, color and detail fade out toward the edge of the focus area.'],'toggle',null,p => nr(p) && p.quadViews === 'QuadViewsFoveated' && p.quadFocusAdapter && p.neuralFocusArea >= 100 && effectiveFovea(p).blend > 0);
  field(c,'neuralWorkingScale','Working scale','The resolution DLSS 5 works at. Lower costs less GPU and gives less detail.','number',[.1,1,.01],nr,'ratio');
  field(c,'neuralBeforeUpscaling','Process at render resolution','On: faster, before DLSS upscaling. Off: at output resolution, more detail, more GPU.','toggle',null,nr);
  field(c,'neuralIntensity','Intensity','How strongly the DLSS 5 image is blended in. Default 1.','number',[0,1,.01],nr,'ratio');
  segmented(field(c,'neuralStyle','Model style','The look of the DLSS 5 model.','select',[['Standard','Standard · default'],['Natural','Natural'],['Cinematic','Cinematic']],nr),{Standard:'Standard',Natural:'Natural',Cinematic:'Cinematic'});
  // The DLSS model DCS upscales with: the focus area first, then around it.
  c = card('dlssPresets','DLSS presets','The DLSS model DCS upscales with, for the focus area and around it.');
  field(c,'focusDlssPreset','Focus DLSS preset','The DLSS model for the focus area. K, L and M are sharper and cost a little more.','select',[[0,'Same as DCS · default'],[11,'K'],[12,'L'],[13,'M'],[5,'E']],p => ((nr(p) || p.foveatedDlss) && p.quadViews === 'QuadViewsFoveated' && p.quadFocusAdapter) || usesPeripheryDlssPreset(p));
  // The engine module's per-view preset (VrProfile.UsesPeripheryDlssPreset); ProfileValidation says what it still needs.
  field(c,'peripheryDlssPreset','Periphery DLSS preset','A lighter model outside the focus area: about 0.3 ms faster, a little more shimmer at the edge. Needs Quad Views, Engine Optimizations and DCS\'s Upscaling on DLSS.','select',[['Off','Off · default'],['E','E (recommended)'],['F','F'],['K','K'],['M','M']],p => !p.desktop && ((p.quadViews === 'QuadViewsFoveated' && p.engineOptimizations) || p.peripheryDlssPreset !== 'Off'));
  // Hidden with Quad Views (updateControls): there DLSS 5 builds on DCS's own DLSS of the focus views.
  c = card('foveatedDlssControls','Foveated Super Resolution','Without Quad Views only. Works with or without DLSS 5.');
  field(c,'foveatedDlss','Use Foveated Super Resolution',['More FPS: full-quality DLSS only where you look.','Lighter around it. For stereo without Quad Views.'],'toggle',null,null,'',p => p.quadViews !== 'None');
  c = card('stereoFocusControls','Focus area without Quad Views','How much of the view is in full detail when Quad Views is off.');
  // Stereo Cheeky (DLSS 5 or Foveated Super Resolution without Quad Views) sizes its own focus as fractions of the view.
  const stereoHidden = p => p.quadViews !== 'None';
  field(c,'foveaWidth','Focus width','How much of the view\'s width is in full detail (1 = all of it).','number',[.1,1,.01],coverageOwned,'ratio',stereoHidden);
  field(c,'foveaHeight','Focus height','How much of the view\'s height is in full detail (1 = all of it).','number',[.1,1,.01],coverageOwned,'ratio',stereoHidden);
  field(c,'peripheralScale','Peripheral resolution','Foveated Super Resolution only: resolution around the focus area. Lower saves GPU.','number',[.15,1,.01],foveatedSr,'ratio',stereoHidden);
  let column = document.createElement('div'); column.className = 'column'; column.id = 'neuralLeft'; $('neuralControls').append(column);
  column = document.createElement('div'); column.className = 'column'; column.id = 'neuralRight'; $('neuralControls').append(column);
  c = card('neuralLeft','Structure & appearance','Defaults: 1×, masks off.');
  field(c,'neuralLocalTone','Local tone','Local contrast response.','number',[0,2,.01],nr,'×');
  field(c,'neuralLocalStructure','Local structure','Fine-detail reconstruction.','number',[0,2,.01],nr,'×');
  field(c,'neuralSkinStructure','Skin structure','Detail on skin; depends on the model mask.','number',[0,2,.01],nr,'×');
  field(c,'neuralAutomaticMask','Automatic mask','Let the model mask regions automatically.','toggle',null,nr);
  field(c,'neuralUiCorrection','UI correction','Model handling of HUD and cockpit text.','toggle',null,nr);
  c = card('neuralRight','Color & transfer','Defaults: 1×.');
  field(c,'neuralColorStrength','Color strength','Color reconstruction strength.','number',[0,2,.01],nr,'×');
  field(c,'neuralTransferStrength','Transfer strength','Transfer response strength.','number',[0,2,.01],nr,'×');
  field(c,'neuralPaperWhiteScale','Paper white scale','White-level multiplier.','number',[.01,8,.01],nr,'×');
  c = card('neuralRight','Depth & motion guides','Defaults: follow the game, 1×. Change only to fix a known guide issue.');
  segmented(field(c,'neuralDepth','Depth convention','Follow the game or force a convention.','select',[['Game','Follow the game · default'],['Normal','Normal depth'],['Reversed','Reversed depth']],nr),{Game:'Game',Normal:'Normal',Reversed:'Reversed'});
  field(c,'neuralMotionScaleX','Motion scale X','Horizontal motion vector multiplier.','number',[-4,4,.01],nr,'×');
  field(c,'neuralMotionScaleY','Motion scale Y','Vertical motion vector multiplier.','number',[-4,4,.01],nr,'×');
  // The static Advanced image controls fold: how many model corrections are away from their defaults.
  { const ones = ['neuralLocalTone','neuralLocalStructure','neuralSkinStructure','neuralColorStrength','neuralTransferStrength','neuralPaperWhiteScale','neuralMotionScaleX','neuralMotionScaleY'];
    foldValues.push({box:$('neuralAdvanced'),value:p => { const n = ones.filter(k => Number(p[k]) !== 1).length + ['neuralAutomaticMask','neuralUiCorrection'].filter(k => p[k]).length + (p.neuralDepth !== 'Game' ? 1 : 0);
      return n ? `${n} changed` : 'All at defaults'; }}); }
  // Frame generation page. Left: how many frames are generated. Right: the DCS frame limit, how generated frames look,
  // then one fold for troubleshooting.
  c = card('framegenControls','Frame generation','Frames added between the ones DCS renders, so the headset gets one at every refresh.');
  segmented(field(c,'frameGen','Method','NVIDIA Optical Flow needs an RTX card; FidelityFX works on any card.','select',[['Off','Off'],['Nvidia','NVIDIA Optical Flow · RTX cards'],['FidelityFx','FidelityFX Optical Flow · any card']]),{Off:'Off',Nvidia:'NVIDIA',FidelityFx:'FidelityFX'});
  segmented(field(c,'frameGenFactor','Generated frames',['Auto: frames are generated only as much as DCS needs.','Without Only when needed, DCS renders half the refresh rate, a third while it falls behind. 2× or 3×: fixed, DCS renders half or a third. 3× adds latency and smearing in sideways motion.'],'select',[[0,'Auto · default'],[2,'2× · one generated frame'],[3,'3× · two generated frames']],p => p.frameGen !== 'Off'),{0:'Auto',2:'2×',3:'3×'});
  field(c,'frameGenAdaptiveCadence','Only when needed',['Frames are generated only while DCS falls behind: lower latency. Needs Generated frames on Auto and Frame limit on Match refresh.','DCS renders the full refresh rate, so latency is lower whenever DCS keeps up.'],'toggle',null,p => p.frameGen !== 'Off' && p.frameGenFactor === 0);
  field(c,'frameGenDeepPipeline','Smoothness buffer',['Smoother with uneven DCS frame times, with a little more latency.','Holds one extra frame: about 11 ms more latency at 90 Hz while frames are generated. In 3× it holds it only while frames keep arriving late.'],'toggle',null,p => p.frameGen !== 'Off');
  // frameGenPipelinedDeepHold has no control: it follows the Smoothness buffer (on with it, off without it) and stays in
  // the profile file only (ConfigurationWriters.Ofxr writes pipelined_deep_hold only with frameGenDeepPipeline).
  // The custom limit right under the select that enables it; the refresh rate source before the value it replaces.
  c = card('pacingControls','DCS frame limit','Set in DCS at launch. Back to stock DCS puts your values back.');
  segmented(field(c,'fpsLimit','Frame limit',['Match refresh renders only the frames the headset needs.','The full refresh rate, or half or a third with 2× or 3×. Keep leaves the current DCS limit; 300 FPS is a very high limit.'],'select',[['Preserve','Keep the current DCS limit'],['MatchRefresh','Match refresh (recommended)'],['RuntimeHeadroom','Very high (300 FPS)'],['Custom','Custom limit']]),{Preserve:'Keep',MatchRefresh:'Match refresh',RuntimeHeadroom:'300 FPS',Custom:'Custom'});
  field(c,'renderedFpsCap','Custom limit','The most frames DCS renders per second.','number',[30,300,.5],p => p.fpsLimit === 'Custom','FPS');
  field(c,'refreshFromHeadset','Read the refresh rate from the headset','Pimax Play: the rate the headset runs at, or ran at last. Sboys: SteamVR\'s rate.','toggle');
  // Its help says which value is used while the rate is read from the headset (updateControls).
  field(c,'headsetRefreshHz','Headset refresh rate','Used when the refresh rate is not read from the headset, or before the headset software has reported one.','number',[50,240,.5],p => !p.refreshFromHeadset || !state?.headsetRefresh,'Hz');
  field(c,'disableDcsVSync','Desktop VSync off','Stops the monitor\'s VSync holding back the frame rate. The headset keeps its own sync.','toggle');
  c = card('pacingControls','Image quality','How generated frames look.');
  // Greyed out until Depth boost runs, but never while it is on (ProfileValidation's hud-symbology-needs says why), so it can be turned off.
  field(c,'frameGenHudSymbology','<span class="tag">EXPERIMENTAL</span> Sharp HUD',['Stops HUD and helmet symbols smearing in generated frames. Needs Depth boost.','They update at DCS\'s own HUD rate (about 20 times a second). Costs about 0.15 ms of GPU and 160 MB of video memory.'],'toggle',null,p => p.frameGen !== 'Off' && (usesDepthBoost(p) || p.frameGenHudSymbology));
  field(c,'smoothCursor','Smooth mouse cursor',['The mouse cursor moves smoothly in generated frames.','It is drawn at its in-between position. Where you click doesn\'t change.'],'toggle',null,p => p.frameGen !== 'Off');
  // NVIDIA Optical Flow only: gone with FidelityFX, greyed out while frame generation is off.
  const fidelity = p => p.frameGen === 'FidelityFx';
  segmented(field(c,'nvidiaFlowScale','Optical flow resolution','The resolution motion is estimated at (NVIDIA only). Higher costs more GPU.','select',[[50,'50% · default'],[75,'75%'],[100,'100% · full']],flow,'',fidelity),{50:'50%',75:'75%',100:'100%'});
  segmented(field(c,'flowPreset','Optical flow quality','How carefully motion is estimated. Slower costs more GPU.','select',[['Fast','Fast'],['Medium','Medium · default'],['Slow','Slow · most analysis']],flow,'',fidelity),{Fast:'Fast',Medium:'Medium',Slow:'Slow'});
  field(c,'bidirectionalFlow','Bidirectional flow','Estimates motion forwards and backwards. Costs more GPU.','toggle',null,flow,'',fidelity);
  more = disclosure(c,'Diagnostics','Keep the defaults unless you are tracking down a problem.',p => `${['showOverlay','diagnosticRecorder','diagnosticFrameRecords','frameGenAsyncArm'].filter(k => p[k]).length} of 4 on`);
  toggles = toggleList(more);
  field(toggles,'showOverlay','FPS counter',['Frame generation\'s own FPS counter, always visible.','The headset panel (In flight page) also shows FPS and can be hidden in flight.'],'toggle',null,p => p.frameGen !== 'Off');
  field(toggles,'diagnosticRecorder','Frame generation log','Writes the frame generation log while you fly, for troubleshooting.','toggle',null,p => p.frameGen !== 'Off');
  field(toggles,'diagnosticFrameRecords','Per-frame records','A line per frame in the log, to find stutters. The log can grow to 1 GB.','toggle',null,p => p.frameGen !== 'Off' && p.diagnosticRecorder);
  field(toggles,'frameGenAsyncArm','Prepare in the background',['Gets ready on another thread when a new view starts.','For example leaving the F10 map; frames pass through meanwhile.'],'toggle',null,p => p.frameGen !== 'Off');
  // CPU Boost page: started with Launch DCS, undone when DCS exits.
  c = card('boostControls','CPU Boost','Applied by Launch DCS, undone when DCS exits.');
  field(c,'cpuBoost','CPU Boost','Gives DCS priority and its best CPU cores; other programs move aside.','toggle');
  segmented(field(c,'boostDcsPriority','DCS priority','Above normal suits most setups. High can starve audio, input and headset services when DCS uses every core.','select',[['Normal','Normal'],['AboveNormal','Above normal · recommended'],['High','High']],boost),{Normal:'Normal',AboveNormal:'Above normal',High:'High'});
  field(c,'boostMoveVrRuntime','Move VR runtime and headset services','Keeps Pimax or SteamVR services off DCS\'s main and render cores.','toggle',null,p => boost(p) && !p.desktop,'',p => p.desktop);
  field(c,'boostMoveBackgroundApps','Move background apps','Moves the apps below to the slowest cores, at below-normal priority, while DCS runs.','toggle',null,boost);
  field(c,'boostBackgroundApps','Background apps','Names as in Task Manager\'s Details tab, one per line, without .exe.','list',null,p => boost(p) && p.boostMoveBackgroundApps);
  field(c,'boostElevated','Administrator rights',['Lets system services be moved too (asks UAC when DCS starts).','DCS itself still runs as your user. Off: those services are left as they are.'],'toggle',null,boost);
  // Flight helpers: each one on its own, with or without CPU Boost; started with Launch DCS, undone when DCS exits.
  // At the top of the right column, above the plan; the VR-only helpers are hidden with Optimizations only.
  c = card('helperControls','Flight helpers','Each works on its own. Started by Launch DCS, undone when DCS exits.');
  field(c,'freeVram','Free VRAM before flight',['Closes the programs below when DCS starts, to free their video memory.','Each gets a normal close request first. No program or Windows setting is changed.'],'toggle');
  const vram = p => p.freeVram;
  field(c,'freeVramApps','Programs to close',['Names as in Task Manager\'s Details tab; * and ? are wildcards.','One per line, without .exe. Voice chat (Discord) and recording (OBS) are left out on purpose.'],'list',null,vram);
  field(c,'freeVramReopen','Reopen after the flight','Starts the closed programs again, as your user, when DCS exits.','toggle',null,vram);
  field(c,'freeVramForce','End programs that do not close','Ends a program that ignores the close request after 5 s; unsaved work in it is lost. Off: it keeps running.','toggle',null,vram);
  field(c,'pauseTobiiDesktop','Pause the desktop Tobii eye tracker',['Stops a desktop Tobii taking over the headset\'s eye tracking.','For a desktop Tobii (Eye Tracker 5 or 4C) next to a Pimax Crystal: it is paused while DCS runs. Asks for administrator rights.'],'toggle',null,p => !p.desktop,'',p => p.desktop);
  field(c,'smallDcsWindow','Small DCS window in VR',['A 1280×720 window on the monitor saves some VRAM and GPU time.','Windowed. The headset image does not change. Back to stock DCS puts your window settings back.'],'toggle',null,p => !p.desktop,'',p => p.desktop);
  field(c,'lowerMonitor','Lower the monitor while flying',['Switches the monitor to the mode below while DCS runs.','It switches back when DCS exits. It is never saved as a Windows setting, so a restart also brings your mode back. HDR is left as it is.'],'toggle',null,p => !p.desktop,'',p => p.desktop);
  field(c,'flightDisplayMode','Monitor mode in flight','Modes your main monitor reports. 1920×1080 at 60 Hz suits most setups.','select',[[displayModeKey(profileDefaults),displayModeLabel(profileDefaults)]],p => p.lowerMonitor && !p.desktop,'',p => p.desktop,
    {prop:'flightDisplayMode',get:p => displayModeKey(p),set:(p,v) => { const [w,h,hz] = v.split(/[x@]/).map(Number); p.flightDisplayWidth = w; p.flightDisplayHeight = h; p.flightDisplayRefresh = hz; }});
  // Engine Optimizations page: the DcsQvCull module, installed in Saved Games with the profile.
  const engine = p => p.engineOptimizations;
  c = card('engineControls','Engine Optimizations',['More FPS from the same CPU; the image does not change.','About 50 to 60% more FPS on a busy airfield in VR, until the GPU is the limit.']);
  field(c,'engineOptimizations','Turn on',['Stays on whenever DCS starts, until you turn it off here.','Also from Steam or a shortcut; Back to stock DCS turns it off too. Nothing in the DCS folder changes. Tested on DCS 2.9.30, single player.'],'toggle');
  field(c,'engineReleaseIdleDlss','Free unused DLSS memory',['Frees the unused DLSS copy each mission load leaves (0.7–1.4 GB).','Helps long sessions with many restarts. The image doesn\'t change. Off by default.'],'toggle',null,engine);
  field(c,'engineShadowCache','<span class="tag">EXPERIMENTAL</span> Cache distant shadows',['About 0.7 ms less GPU time; distant tree shadows sway less smoothly.','Reuses the distant shadows (about 70 m to 1.5 km) for a few frames. Off by default.'],'toggle',null,engine);
  // The parts, biggest measured gain first, folded: all on is the tested set.
  // The fold counts like the Engine row of the right panel: the terrain prefetch fix is one more part when it skips repeats.
  more = disclosure(c,'Individual optimizations','All on is best. Turn one off only if a problem points to it.',p => `${ENGINE_PARTS.filter(k => p[k]).length + (p.boostPrefetch === 'Skip' ? 1 : 0)} of ${ENGINE_PARTS.length + 1} on`);
  more = toggleList(more);
  field(more,'engineGBufferRecorder','Multi-threaded G-buffer','Prepares the scene on other CPU cores. Same image. About 22% more FPS on its own.','toggle',null,engine);
  field(more,'engineModelAllocator','Model data allocator (parallel)','Gives each model thread its own memory, so they stop waiting for each other. About 6% more FPS.','toggle',null,engine);
  field(more,'engineShaderTimeCache','Streaming timer cache','Reads the clock less often while streaming textures. About 5% more FPS.','toggle',null,engine);
  field(more,'engineTaskQueueClock','Task queue clock cache (camera motion)','The same for the task queue at the start of each frame. Helps when the view moves fast. Needs Streaming timer cache.','toggle',null,p => engine(p) && p.engineShaderTimeCache);
  segmented(field(more,'boostPrefetch','Terrain prefetch fix','Skips repeated terrain memory hints. About 4% less CPU. Observe only counts them.','select',[['Off','Off'],['Observe','Observe · count only'],['Skip','Skip repeats · recommended']],engine),{Off:'Off',Observe:'Observe',Skip:'Skip'});
  field(more,'engineShadowRecorder','Multi-threaded shadows','Prepares the shadows on other CPU cores. Same image. 3 to 5% more FPS; total CPU use rises a little.','toggle',null,engine);
  field(more,'engineShadowInstancing','Shadow caster instancing (exact)','Draws identical shadow casters together. Same image. About 3% more FPS, from about 2.5 minutes after DCS starts.','toggle',null,engine);
  field(more,'engineCostWeights','Culling partition by real cost','Splits the search for visible objects by what each object really costs. About 3% more FPS.','toggle',null,engine);
  field(more,'enginePartitionBoost','Culling partition boost','Splits the search for visible objects into more, even parts. About 2% more FPS.','toggle',null,engine);
  field(more,'engineTextureDedupe','Texture streaming dedupe','Skips repeated requests for the same texture. About 2% more FPS.','toggle',null,engine);
  field(more,'engineEffectBufferSkip','Effect constant-buffer skip','Skips effect data that is already set. Small gain.','toggle',null,engine);
  field(more,'enginePlainCounter','Statistics counter without lock','Removes a wait between model threads. Small gain.','toggle',null,engine);
  field(more,'engineFrameHeap','Frame memory per thread','Gives each thread its own short-lived memory. About 1.5% more FPS.','toggle',null,engine);
  field(more,'engineStateFilter','Redundant state filter','Skips graphics state that is already set. Same image. About 1% more FPS. Turns itself off after a DCS update.','toggle',null,engine);
  field(more,'engineBeeps','Beeps for the in-flight switch','One beep off, two beeps on.','toggle',null,engine);
  more = disclosure(c,'Developer options','For working on the module. Leave as they are for flying.',p => p.engineDevMode ? 'Developer mode on' : 'Off');
  field(more,'engineTimerRefreshUs','Streaming timer refresh','How often the cached time is refreshed. 1000 µs (1 ms) is far below one frame.','number',[250,2000,50],p => engine(p) && p.engineShaderTimeCache,'µs');
  field(more,'engineDevMode','Developer mode',['For working on the module; off for flying.','On: DCS hot-reloads the payload below whenever it changes and takes every engine setting from the settings file below instead of this page. Off for normal flying and for testing a release.'],'toggle',null,engine);
  field(more,'engineDevPayloadPath','Developer payload','A DcsQvCullPayload.dll built elsewhere. While it is missing, the installed copy runs.','path',null,p => engine(p) && p.engineDevMode);
  field(more,'engineDevIniPath','Developer settings file','A DcsQvCull.ini that replaces the one this profile installs. A run_suite.flag next to it starts the test suite too.','path',null,p => engine(p) && p.engineDevMode);
  field(more,'engineDiagnosticHooks','Measurement hooks always on','Normally installed only while the test suite runs. For troubleshooting; costs some CPU.','toggle',null,engine);
  // DCS & headset page.
  c = card('gameControls','DCS','Detected automatically; change only if DCS is somewhere else.');
  field(c,'dcs','DCS executable','DCS.exe in the bin folder of your installation.','path');
  field(c,'options','DCS settings file','options.lua in Saved Games\\DCS\\Config.','path');
  field(c,'keepDcsLauncher','Show the DCS launcher',['On: the DCS launcher opens first. Off: straight into the game.','With the launcher, press Play there. Back to stock DCS turns the launcher back on.'],'toggle');
  // Pupil shift (native/pupil_shift): an OpenXR layer first in the chain; it needs the gaze of eye-tracked Quad Views.
  const pupil = p => p.pupilShift;
  // Perception page: the eye distance DCS renders with first, since Pupil shift and Depth boost build on it.
  c = card('eyeControls','Eye distance','Sets the size and depth of the world.');
  segmented(field(c,'eyeDistance','Eye distance',['The headset\'s own value gives the world its true size and depth. DCS\'s custom IPD changes the size of everything.','Measured, or set in Pimax Play or SteamVR. Leave DCS\'s custom IPD off unless you have a reason. Back to stock DCS restores DCS\'s own setting.'],'select',[['Headset','The headset\'s own (recommended)'],['DcsCustom','DCS\'s custom IPD (as set in DCS)'],['Manual','Set here']],p => !p.desktop),{Headset:'Headset\'s own',DcsCustom:'DCS custom IPD',Manual:'Set here'});
  field(c,'eyeDistanceMm','Eye distance value','Used when Eye distance is Set here.','number',[50,80,.5],p => !p.desktop && p.eyeDistance === 'Manual','mm');
  { const ipd = document.createElement('p'); ipd.className = 'pimax-note card-note'; ipd.id = 'pupilIpd'; ipd.hidden = true; c.append(ipd); }
  c = card('pupilControls','<span class="tag">EXPERIMENTAL</span> Pupil shift',['Each eye is rendered from where its pupil is, so the cockpit stays still when only your eyes move.','Your pupils move a little when your eyes turn. If the cockpit seems to move, turn it off.']);
  needsNote(c,'pupilNeeds');
  field(c,'pupilShift','Pupil shift','Each eye is rendered from where its pupil is. No GPU cost.','toggle');
  field(c,'pupilShiftEyeRadiusMm','Eye rotation radius',['10 to 11 mm fits most people.','How far the pupil sits from the point the eye turns around.'],'number',[5,15,.5],pupil,'mm');
  more = disclosure(c,'Advanced','Rarely needs changing.',() => shown('pupilShiftVirtualImageM'));
  field(more,'pupilShiftVirtualImageM','Lens image distance','How far away the lens shows the image. 1 to 2 m is typical; it barely matters.','number',[.5,5,.1],pupil,'m');
  // Periphery stability (native/quadviews/dcsvr_perception.h): a pass in the bundled Quad Views, fixed or eye-tracked focus.
  const stability = p => p.peripheryStability;
  c = card('stabilityControls','<span class="tag">EXPERIMENTAL</span> Periphery stability',['Calms shimmering thin lines around the focus area. Bundled Quad Views only.','Canopy frames, power lines and the horizon. The focus area itself is not touched.']);
  field(c,'peripheryStability','Periphery stability','Steadies the periphery from frame to frame. Almost no GPU cost.','toggle');
  field(c,'peripheryStabilityStrength','Strength','Higher is steadier; 0.5 is a good start. Moving scenery can leave a faint trail.','number',[0,1,.05],stability,'ratio');
  // Depth boost (same layer, its own switch): the render IPD rises while you look far and returns in the cockpit.
  const depth = p => p.depthBoost, realistic = p => p.depthBoost && p.depthBoostMode !== 'Enhanced', enhanced = p => p.depthBoost && p.depthBoostMode === 'Enhanced';
  // Only the selected mode's strength and distances are shown, so the card never shows two Strength sliders.
  const enhancedMode = p => p.depthBoostMode === 'Enhanced';
  c = card('depthControls','<span class="tag">EXPERIMENTAL</span> Depth boost','More depth for the world outside, following where you look. Needs eye tracking.');
  needsNote(c,'depthNeeds');
  field(c,'depthBoost','Depth boost',['More depth where you look; the cockpit keeps your real eye distance. Keep it off for landings for now.','The world is rendered with the eyes a bit further apart, changing slowly. Cockpit displays, lighting and the HUD are unchanged.'],'toggle');
  segmented(field(c,'depthBoostMode','Mode',['Realistic: more depth beyond 150 m. Enhanced: from 3 m, stronger.','Realistic adds depth only where the headset shows less than your eyes would. Enhanced is not realistic: the world looks smaller.'],'select',[['Realistic','Realistic (recommended)'],['Enhanced','Enhanced: more depth, not realistic']],depth),{Realistic:'Realistic',Enhanced:'Enhanced'});
  field(c,'depthBoostRealisticMax','Strength','How much more depth far away. ×1.25 turns an eye distance of 64 mm into 80 mm.','number',[1,1.5,.05],realistic,'×',enhancedMode);
  field(c,'depthBoostMax','Strength','How much more depth from 50 m on. ×1.4 turns an eye distance of 64 mm into 90 mm.','number',[1,2,.05],enhanced,'×',p => !enhancedMode(p));
  more = disclosure(c,'Advanced','Distances, and your own aircraft. The defaults suit most pilots.',p => (p.depthBoostMode === 'Enhanced' ? '' : `${shown('depthBoostFarStartM')} → ${shown('depthBoostFarFullM')} · `) + 'cockpit ' + shown('depthBoostNearM'));
  field(more,'depthBoostFarStartM','Normal depth up to','Nothing changes closer than this.','number',[50,1000,10],realistic,'m',enhancedMode);
  field(more,'depthBoostFarFullM','Full strength from','Full strength from this distance on; in between it grows gradually.','number',[60,3000,10],realistic,'m',enhancedMode);
  field(more,'depthBoostNearM','Cockpit distance','Looking closer than this counts as looking at the cockpit: the world keeps the depth it had. The cockpit itself always keeps your real eye distance.','number',[.5,5,.1],depth,'m');
  field(more,'ownAircraftTrueDepth','Own aircraft at true depth','Helicopters and two-seaters: the other crew station or the front seat outside your cockpit stays at its real distance instead of looking closer. Still being checked on every aircraft.','toggle',null,depth);
  // Far depth (docs/FAR_DEPTH.md): bundled Quad Views shifts each eye's image with the depth the engine module measures.
  c = card('farDepthControls','<span class="tag">EXPERIMENTAL</span> Far depth',['More depth for distant aircraft, ships and terrain. No eye tracking needed.','From 50 m to 2 km. Works alone or with Depth boost.']);
  needsNote(c,'farDepthNeeds');
  field(c,'farDepth','Far depth','The cockpit stays exactly as it is. Costs under 0.1 ms per frame.','toggle');
  segmented(field(c,'farDepthPreset','Strength','Start with Default.','select',[['Subtle','Subtle'],['Default','Default'],['Strong','Strong']],p => p.farDepth),{Subtle:'Subtle',Default:'Default',Strong:'Strong'});
  // Visual realism page: the engine module's color modes and [VrFx] effects (installed for them alone too, in its
  // Perception-only mode). Main controls in the cards, the rest in Advanced folds.
  const vrOnly = p => !p.desktop;
  c = card('colorControls','Color','How DCS looks in the headset. Its key (In flight page) switches modes.');
  segmented(field(c,'colorMode','Color mode','Off is DCS\'s own image. Fix: true blacks, even eyes (0.05 ms). Enhanced adds filmic color, night vision and glare (0.1 ms).','select',[['Off','Off · DCS\'s own image'],['Fix','Fix · true blacks, even eyes'],['Enhanced','Enhanced · Fix plus filmic color, night vision, glare']],vrOnly),{Off:'Off',Fix:'Fix',Enhanced:'Enhanced'});
  segmented(field(c,'panelGamut','Panel color correction',['For the Crystal Super\'s over-saturated wide-gamut panels.','Needs Fix or Enhanced. Full corrects this, Half corrects half. HUD and MFDs are not affected. Don\'t combine with Pimax\'s own color correction.'],'select',[[0,'Off · default'],[1,'Full'],[2,'Half']],colorOn),{0:'Off',1:'Full',2:'Half'});
  // Only the chosen mode's fine-tuning shows; both while the color mode key can switch modes in flight.
  const fixHidden = p => p.colorMode !== 'Fix' && !usesColorKey(p), enhancedHidden = p => p.colorMode !== 'Enhanced' && !usesColorKey(p);
  more = disclosure(c,'Fine-tune Fix','The defaults are the module\'s own.',() => `${shown('colorFixExposureEv')} · ${shown('colorFixSaturation')}`); more.dataset.hide = 'fix';
  field(more,'colorFixExposureEv','Brightness','Overall brightness.','number',[-2,2,.1],fixOn,'EV',fixHidden);
  field(more,'colorFixNightEv','Night brightness','Extra brightness at night only. Raise it if nights are too dark.','number',[-2,2,.1],fixOn,'EV',fixHidden);
  field(more,'colorFixSaturation','Saturation','Color intensity. Brightness stays the same.','number',[.5,1.5,.05],fixOn,'×',fixHidden);
  field(more,'colorFixWarmth','Golden-hour warmth','Warmer light near sunrise and sunset only.','number',[0,1,.05],fixOn,'ratio',fixHidden);
  field(more,'colorFixAdaptBright','Eye adaptation to bright','How slowly your eyes adjust to brighter light, on top of DCS. 0 = DCS only.','number',[0,10,.1],fixOn,'s',fixHidden);
  field(more,'colorFixAdaptDark','Eye adaptation to dark','How slowly your eyes adjust to the dark, on top of DCS. 0 = DCS only.','number',[0,30,.5],fixOn,'s',fixHidden);
  more = disclosure(c,'Fine-tune Enhanced','The defaults are the module\'s own.',() => `${shown('colorEnhancedExposureEv')} · ${shown('colorEnhancedSaturation')}`); more.dataset.hide = 'enhanced';
  field(more,'colorEnhancedExposureEv','Brightness','Overall brightness.','number',[-2,2,.1],enhancedOn,'EV',enhancedHidden);
  field(more,'colorEnhancedNightEv','Night brightness','Extra brightness at night only. Raise it if nights are too dark.','number',[-2,2,.1],enhancedOn,'EV',enhancedHidden);
  field(more,'colorEnhancedSaturation','Saturation','Color intensity. Brightness stays the same.','number',[.5,1.5,.05],enhancedOn,'×',enhancedHidden);
  field(more,'colorEnhancedWarmth','Golden-hour warmth','Warmer light near sunrise and sunset only.','number',[0,1,.05],enhancedOn,'ratio',enhancedHidden);
  field(more,'colorEnhancedAdaptBright','Eye adaptation to bright','How slowly your eyes adjust to brighter light, on top of DCS. 0 = DCS only.','number',[0,10,.1],enhancedOn,'s',enhancedHidden);
  field(more,'colorEnhancedAdaptDark','Eye adaptation to dark','How slowly your eyes adjust to the dark, on top of DCS. 0 = DCS only.','number',[0,30,.5],enhancedOn,'s',enhancedHidden);
  c = card('enhancedControls','Night vision and glare','Seeing in the dark, and halos around bright lights.');
  needsNote(c,'enhancedNeeds');
  field(c,'nightVision','Night vision',['At night, dim areas lose color, as in moonlight. About 0.08 ms.','They turn slightly blue-grey. Lights, lit instruments and the HUD keep their color. No effect by day.'],'toggle',null,enhancedOn);
  field(c,'nightVisionStrength','Night vision strength','0 looks like DCS; 1 is full moonlight vision.','number',[0,1,.05],p => enhancedOn(p) && p.nightVision,'ratio');
  field(c,'glare','Light glare',['A soft halo around very bright lights. About 0.1 ms.','The sun, landing lights, strobes, fires and afterburners, as through a canopy.'],'toggle',null,enhancedOn);
  field(c,'glareStrength','Glare strength','How strong the halo is.','number',[0,1,.05],p => enhancedOn(p) && p.glare,'ratio');
  more = disclosure(c,'Advanced','The defaults are the module\'s own.',() => `blue ${shown('nightVisionBlue')} · halo ${shown('glareRadius')}`);
  field(more,'nightVisionBlue','Blue tint','How blue night shadows look. 0 = neutral grey.','number',[0,1,.05],p => enhancedOn(p) && p.nightVision,'ratio');
  field(more,'nightVisionBlur','Night softness','Slight loss of fine detail in the dark, as your eyes have at night. 0 = sharp.','number',[0,1,.05],p => enhancedOn(p) && p.nightVision,'ratio');
  field(more,'nightVisionScotopic','Full-night level','Light level at and below which you see in full night vision. Higher starts it earlier at dusk.','number',[.0005,.5,.0005],p => enhancedOn(p) && p.nightVision,'cd/m²');
  field(more,'nightVisionPhotopic','Daylight level','Light level at and above which you see in full daylight vision. At least 4 times the full-night level.','number',[.05,100,.05],p => enhancedOn(p) && p.nightVision,'cd/m²');
  field(more,'glareThresholdEv','Glare threshold','How much brighter than the scene a light must be to glare. Lower: more lights glare.','number',[0,8,.5],p => enhancedOn(p) && p.glare,'EV');
  field(more,'glareRadius','Halo size','Size of the halo, in degrees of view.','number',[1,20,1],p => enhancedOn(p) && p.glare,'°');
  field(more,'glareStreak','Star streaks','Share of the glare drawn as thin six-pointed rays. 0 = round halo only.','number',[0,1,.05],p => enhancedOn(p) && p.glare,'ratio');
  // G vision first: every VR pilot can use it; Eye-focus blur needs eye tracking.
  c = card('gVisionControls','G vision','Under high G, color fades and vision closes in, then goes black. Red-out under negative G.');
  field(c,'gVision','Realistic G vision loss',['Grey-out, tunnel vision and blackout under G, centered on your gaze.','Works with DCS\'s own G effect without doubling it. About 0.04 ms, only while G is high.'],'toggle',null,vrOnly);
  field(c,'gVisionStrength','Effect strength','How strong the color loss, tunnel and darkening are. 1 allows a full blackout.','number',[0,1,.05],gVisionOn,'ratio');
  more = disclosure(c,'Advanced','The defaults are the module\'s own.',() => `${shown('gVisionMode')} · ${shown('gVisionTolerance')}`);
  segmented(field(more,'gVisionMode','Mode',['Auto follows DCS\'s G-EFFECT option.','Complement: DCS darkens, this adds the tunnel and color loss. Full: this does all of it, for DCS\'s G-EFFECT set to None (which also turns off DCS\'s G-LOC).'],'select',[['Auto','Auto · default'],['Complement','Complement DCS'],['Full','Full']],gVisionOn),{Auto:'Auto',Complement:'Complement',Full:'Full'});
  field(more,'gVisionTolerance','G tolerance','The sustained G at which you black out; grey-out starts about 1.2 G lower. 7.5: g-suit and straining. About 5: relaxed, no g-suit.','number',[4,10,.5],gVisionOn,'G');
  field(more,'gVisionRedout','Red-out (negative G)','A red tint below about −2 G, when G vision does all of it (Full, or Auto with DCS\'s G effect off).','toggle',null,gVisionOn);
  field(more,'gVisionUseGaze','Follow my eyes','The tunnel moves with your eyes. Needs Quad Views with eye tracking; without it the tunnel stays straight ahead.','toggle',null,gVisionOn);
  c = card('gazeDofControls','<span class="tag">EXPERIMENTAL</span> Eye-focus blur','Nearer or farther things go slightly soft, like a real eye. Needs Quad Views with eye tracking.');
  needsNote(c,'gazeDofNeeds');
  field(c,'gazeDof','Eye-focus blur',['Subtle, and clears at once when your eyes move. Some people find it uncomfortable. About 0.18 ms.','Blurs what is at a different distance from the point you look at. Try it for a few minutes.'],'toggle',null,p => eyeTrackedVr(p) || Boolean(p.gazeDof));
  field(c,'gazeDofStrength','Blur strength','0.5 is subtle; 1 is what a real eye would see.','number',[0,1,.05],usesGazeDof,'ratio');
  more = disclosure(c,'Advanced','The defaults are the module\'s own.',() => `${shown('gazeDofMaxArcmin')} · settle ${shown('gazeDofSettleMs')}`);
  field(more,'gazeDofMaxArcmin','Maximum blur','Upper limit of the blur, in arc minutes. Raise it for a stronger effect at night.','number',[1,20,1],usesGazeDof,'arcmin');
  field(more,'gazeDofSaccadeRelease','Sharp during eye movements','Recommended. Off keeps the blur through quick eye movements at the same distance.','toggle',null,usesGazeDof);
  field(more,'gazeDofSettleMs','Settle time','How long everything stays sharp after you look somewhere new.','number',[0,500,10],usesGazeDof,'ms');
  field(more,'gazeDofFadeInMs','Blur fade-in','How gently the blur comes back after the settle time. Longer is less noticeable.','number',[50,1000,10],usesGazeDof,'ms');
  field(more,'gazeDofToleranceD','Depth of focus','Distance range kept sharp around where you look, in dioptres. Larger: less blur.','number',[.1,1,.05],usesGazeDof,'D');
  field(more,'gazeDofFoveaGuardDeg','Sharp zone around gaze','Nothing within this angle of where you look is ever blurred.','number',[0,5,.5],usesGazeDof,'°');
  field(more,'gazeDofPupilMm','Pupil size','0 = automatic from the scene brightness (about 2.5 mm by day, 6.5 mm at night), else 2 to 8 mm. A larger pupil blurs more.','number',[0,8,.5],usesGazeDof,'mm');
  // The in-headset panel (OFXR): with frame generation always; without it OFXR is loaded only to draw the panel.
  const panelOn = p => !p.desktop && (p.frameGen !== 'Off' || p.headsetPanel);
  c = card('panelControls','Headset panel (OSD)',['A small status panel in the headset. Its key shows or hides it in flight.','It shows FPS, DLSS 5, Engine Optimizations, Pupil shift, Depth boost and Far depth.']);
  field(c,'headsetPanel','Headset panel','Without Frame generation, turn this on to show the panel. Your frames are not touched.','toggle',null,p => !p.desktop && p.frameGen === 'Off','',p => p.frameGen !== 'Off');
  { const always = document.createElement('p'); always.className = 'fine'; always.id = 'headsetPanelAlways'; always.textContent = 'The headset panel is always on with Frame generation.'; $('setting-headsetPanel').after(always); }
  field(c,'diagnosticOverlayAtStart','Visible at start','Each flight starts with the panel shown.','toggle',null,panelOn);
  field(c,'diagnosticVram','VRAM counter','Adds the video memory in use to the panel.','toggle',null,panelOn);
  // Every in-flight key in one card, in the rail's order: capture fields, greyed out while their feature is off in this
  // profile. With Optimizations only, the VR keys are hidden: only the Engine Optimizations key works on a monitor.
  const vrKey = p => p.desktop;
  c = card('keysControls','In-flight keys','Click a key, then press the new combination. Esc cancels.');
  field(c,'diagnosticOverlayKey','Headset panel','Shows or hides the headset panel.','hotkey',null,HOTKEYS.diagnosticOverlayKey.used,'',vrKey);
  field(c,'engineToggleKey','Engine Optimizations','Off and on, to compare. Beeps: one = off, two = on.','hotkey',null,HOTKEYS.engineToggleKey.used);
  field(c,'neuralToggleKey','DLSS 5','Off and on, to compare.','hotkey',null,HOTKEYS.neuralToggleKey.used,'',vrKey);
  field(c,'pupilShiftToggleKey','Pupil shift','Off and on, to compare. Beeps: low = off, high = on.','hotkey',null,HOTKEYS.pupilShiftToggleKey.used,'',vrKey);
  field(c,'depthBoostToggleKey','Depth boost','Off and on, to compare. Beeps: low = off, high = on.','hotkey',null,HOTKEYS.depthBoostToggleKey.used,'',vrKey);
  field(c,'farDepthToggleKey','Far depth','Off and on, to compare. Beeps: one = off, two = on.','hotkey',null,HOTKEYS.farDepthToggleKey.used,'',vrKey);
  field(c,'colorModeKey','Color mode','Cycles Off, Fix and Enhanced (Visual realism page). Beeps: 1, 2 or 3. Alt+Shift+F9 is free.','hotkey',null,HOTKEYS.colorModeKey.used,'',vrKey);
  { const fixed = document.createElement('p'); fixed.className = 'fine'; fixed.textContent = 'Taken by Engine Optimizations: Ctrl+Alt with F8 to F12, Page Up or Page Down.'; c.append(fixed); }
  // The headset software this profile uses and what runs now; the Sboys driver (Sboys route only), then the override.
  c = card('headsetControls','Headset software','Follows How you fly in the right panel: Pimax Play, or SteamVR for Sboys.');
  { const line = document.createElement('p'); line.className = 'headset-active'; line.id = 'headsetActive'; c.append(line, $('sboysCard')); }
  more = disclosure(c,'Runtime override','Only for an OpenXR runtime that is not registered in Windows.',p => p.runtimeManifestPath ? p.runtimeManifestPath.split(/[\\/]/).pop() : 'Registered runtime');
  field(more,'runtimeManifestPath','OpenXR runtime file','The JSON file of an OpenXR runtime to use instead of the registered one. Normally leave it empty. Not used with Optimizations only.','path',null,p => !p.desktop);
  // Why a greyed-out setting waits (field()'s why), for the fields that do not set one: what to turn on or choose first.
  const because = (test,text) => fields.filter(test).forEach(f => { if (!f.why) f.why = text; }), at = (...paths) => f => paths.includes(f.path), fgOff = p => p.frameGen === 'Off';
  const qvWhy = p => p.quadViews !== 'QuadViewsFoveated' ? 'Choose Bundled Quad Views as the provider to change these.' : 'Not with an alternative Quad Views provider.', bundledWhy = 'Needs the bundled Quad Views (Quad Views page).';
  const dlssWhy = 'Turn on DLSS 5 above to change these.', fgWhy = 'Choose a Frame generation method to change these.', engineWhy = 'Turn on Engine Optimizations to change these.', boostWhy = 'Turn on CPU Boost to change these.';
  const enhancedWhy = 'Choose Enhanced in Color mode to change these.';
  because(at('quadSaccadeLead','quadGazeStabilize'),p => !quad(p) ? qvWhy(p) : 'Needs Focus movement: Eye tracked.');
  because(f => f.condition === quad,qvWhy);
  because(at('neuralFocusArea'),p => !nr(p) ? dlssWhy : p.quadViews !== 'QuadViewsFoveated' ? bundledWhy : null);
  because(at('quadNeuralEdgeFade'),p => !nr(p) ? dlssWhy : p.quadViews !== 'QuadViewsFoveated' ? bundledWhy : p.neuralFocusArea < 100 ? 'Set DLSS 5 area to Whole focus area to use this.' : effectiveFovea(p).blend > 0 ? null : 'Needs Focus edge blending above 0 (Quad Views page).');
  because(f => f.condition === nr,dlssWhy);
  because(at('keepOtherDxgi'),'Turn on DLSS 5 or Foveated Super Resolution to change this.');
  // Focus area without Quad Views: sized by DLSS 5 or Foveated Super Resolution; the periphery by Foveated Super Resolution only.
  const stereoWhy = 'Turn on DLSS 5 or Foveated Super Resolution to change these.';
  because(at('peripheralScale'),p => stereoCheeky(p) ? 'Turn on Foveated Super Resolution to change this.' : stereoWhy);
  because(f => f.condition === coverageOwned,stereoWhy);
  because(at('frameGenAdaptiveCadence'),p => fgOff(p) ? fgWhy : 'Set Generated frames to Auto to use this.');
  because(at('frameGenHudSymbology'),p => fgOff(p) ? fgWhy : ['Needs Depth boost (Perception page).','depthBoost']);
  because(at('diagnosticFrameRecords'),p => fgOff(p) ? fgWhy : 'Turn on Frame generation log to use this.');
  because(f => f.condition === flow,p => fgOff(p) ? fgWhy : 'NVIDIA Optical Flow only.');
  because(at('frameGenFactor','frameGenDeepPipeline','smoothCursor','frameGenAsyncArm','showOverlay','diagnosticRecorder'),fgWhy);
  because(at('renderedFpsCap'),'Choose Custom limit in Frame limit to use this.');
  because(at('engineTaskQueueClock','engineTimerRefreshUs'),p => !engine(p) ? engineWhy : 'Turn on Streaming timer cache to use this.');
  because(at('engineDevPayloadPath','engineDevIniPath'),p => !engine(p) ? engineWhy : 'Turn on Developer mode to use this.');
  because(f => f.condition === engine,engineWhy);
  because(at('boostMoveVrRuntime'),p => !boost(p) ? boostWhy : 'Not with Optimizations only.');
  because(at('boostBackgroundApps'),p => !boost(p) ? boostWhy : 'Turn on Move background apps to edit this list.');
  because(f => f.condition === boost,boostWhy);
  because(f => f.condition === vram,'Turn on Free VRAM before flight to change these.');
  because(at('eyeDistanceMm'),'Choose Set here in Eye distance to use this.');
  because(f => f.condition === pupil,'Turn on Pupil shift to change these.');
  because(f => f.condition === stability,'Turn on Periphery stability to change this.');
  because(f => [depth,realistic,enhanced].includes(f.condition),'Turn on Depth boost to change these.');
  because(at('farDepthPreset'),'Turn on Far depth to change this.');
  because(at('panelGamut'),'Choose Fix or Enhanced in Color mode to use this.');
  // Night vision and glare, and Eye-focus blur, say what they need in the note at the top of their card.
  because(at('nightVision','glare','gazeDof'),() => null);
  because(f => f.condition === fixOn,'Choose Fix in Color mode to change these.');
  because(f => f.condition === enhancedOn,enhancedWhy);
  because(f => f.path.startsWith('nightVision'),p => !enhancedOn(p) ? null : 'Turn on Night vision to change these.');
  because(f => f.path.startsWith('glare'),p => !enhancedOn(p) ? null : 'Turn on Light glare to change these.');
  because(f => f.condition === usesGazeDof,p => !eyeTrackedVr(p) ? null : 'Turn on Eye-focus blur to change these.');
  because(f => f.condition === gVisionOn,'Turn on G vision to change these.');
  because(at('headsetPanel'),'In VR only.');
  because(f => f.condition === panelOn,p => p.desktop ? 'In VR only.' : 'Turn on the headset panel, or Frame generation, to change these.');
  const keyFeature = {diagnosticOverlayKey:'the headset panel',engineToggleKey:'Engine Optimizations',neuralToggleKey:'DLSS 5',pupilShiftToggleKey:'Pupil shift',farDepthToggleKey:'Far depth',depthBoostToggleKey:'Depth boost'};
  fields.filter(f => f.type === 'hotkey' && !f.why).forEach(f => f.why = keyFeature[f.path] ? `Used when ${keyFeature[f.path]} is on.` : 'Used in VR only.');
}
function buildFeatureList() {
  // The optimizations every pilot can use, then the VR features under their own label (hidden with Optimizations only).
  const groupLabel = key => key === 'engine' ? '<div class="feature-group">Every pilot</div>' : key === 'quad' ? '<div class="feature-group" id="featureGroup-vr">VR</div>' : '';
  $('featureList').innerHTML = FEATURES.map(([key,title,,target]) => groupLabel(key) + `<div class="feature" id="feature-${key}"><label class="feature-check"><input type="checkbox" id="featureCheck-${key}" data-feature="${key}" aria-describedby="featureSummary-${key}"><span class="feature-text"><b>${title}</b><small class="feature-summary" id="featureSummary-${key}"></small></span></label><button type="button" class="feature-open" data-open-page="${target}" aria-label="Open ${title} settings" title="Open ${title} settings">›</button><div class="feature-notes" id="featureNotes-${key}"></div></div>`).join('');
  $('featureList').addEventListener('change',e => {
    const key = e.target.dataset?.feature; if (!key || busy) return;
    setFeature(profile,key,e.target.checked); change(); if (key === 'boost') boostChanged();
  });
}
function navigate(id) {
  // The VR pages are hidden with Optimizations only: whatever leads there (Ctrl+number, a link, search) opens the overview.
  if (profile?.desktop && VR_PAGES.includes(id)) id = 'overview';
  const entry = pages.find(p => p[0] === id); if (!entry) return; page = id;
  pages.forEach(p => { $('page-'+p[0]).hidden = p[0] !== id; $('nav-'+p[0]).classList.toggle('selected',p[0] === id); $('nav-'+p[0]).setAttribute('aria-current',p[0] === id ? 'page' : 'false'); });
  $('pageTitle').textContent = entry[1]; $('pageDescription').textContent = entry[2]; $('scrollArea').scrollTop = 0;
  if (id === 'foveation' && profile) rereadPimax(false);
  if (id === 'boost' && profile) refreshBoostPlan(0);
  if (id === 'engine' && profile) refreshEngine();
  if (id === 'pupil' && profile) { renderPupilNeeds(); refreshPupil(); }
  if (id === 'foveation' && profile) renderStabilityNeeds();
  if (id === 'visual' && profile) renderVisualNeeds();
  if (id === 'support' && profile) refreshDlssFocus();
  if (id === 'guide') renderGuide();
  if (id === 'overview' && profile) loadFlight();
}
// ---- Pimax Play focus values ------------------------------------------------------------------------------------
async function rereadPimax(announce) {
  try {
    const result = await request('pimaxFovea'); if (!state) return;
    const before = state.pimax?.stamp ?? null; state.pimax = result.pimax;
    // A file list made with other Pimax values is out of date; Launch DCS reads the values again anyway.
    if (plan && pimaxSource(profile) && (plan.foveaStamp ?? null) !== (result.pimax.stamp ?? null)) { plan = null; request('changed').catch(()=>{}); renderDiagnostics(); $('status').textContent = "Pimax Play's Quad View values changed. Launch DCS uses the new ones."; }
    else if (announce) $('status').textContent = result.pimax.found ? (before === (result.pimax.stamp ?? null) ? 'Pimax Play values re-read; unchanged.' : 'Pimax Play values re-read. ' + result.pimax.summary) : "Pimax Play's Quad View settings were not found.";
    updateControls();
  } catch (error) { if (announce) showError(error); }
}
function renderPimax() {
  const box = $('pimaxFovea'), pimax = state?.pimax, mode = profile.quadViews === 'PimaxNative' ? 'native' : pimaxSource(profile) ? 'bundled' : null;
  box.hidden = !mode; if (!mode || !pimax) return;
  const reread = '<button type="button" class="button quiet" id="rereadPimax">Re-read</button>';
  if (!pimax.found) { box.innerHTML = `<div class="pimax-head"><div><b>Pimax Play settings not found</b><small>${escape(pimax.path)}</small></div>${reread}</div><p class="fine">${mode === 'bundled' ? "This profile's own focus values are used until Pimax Play's settings can be read. Choose Set here to edit them." : 'Pimax applies its own values.'}</p>`; return; }
  // Exactly what Pimax Play's Quad View page shows for the selected mode: its labels, its order, its rounding.
  const open = Boolean(box.querySelector('details.pimax-details')?.open);
  const controls = `<dl class="pimax-controls" aria-label="Pimax Play Quad View settings">${pimax.controls.map(x => `<dt>${escape(x.label)}</dt><dd>${escape(x.value)}</dd>`).join('')}</dl>`;
  const warnings = [pimax.mismatch, mode === 'bundled' ? pimax.capNote : null].filter(Boolean).map(n => `<p class="pimax-note warn">${escape(n)}</p>`).join('');
  let how = `<p class="fine">Pimax native Quad Views applies these values itself. Nothing is written to Pimax.</p>`;
  if (mode === 'bundled') {
    const c = pimax.converted, e = effectiveFovea(profile), r = pimax.eyeResolution, share = v => num(v*100)+'%';
    const pixels = r ? `<dt>Focus per eye at ${r.width} × ${r.height}</dt><dd>${r.focusWidth} × ${r.focusHeight} px</dd>` + (c.clamped ? `<dt>Requested focus per eye</dt><dd>${r.pimaxFocusWidth} × ${r.pimaxFocusHeight} px</dd>` : '') : '';
    const notes = pimax.notes.filter(n => n !== pimax.mismatch && n !== pimax.capNote).map(n => `<p class="pimax-note">${escape(n)}</p>`).join('');
    how = `<details class="pimax-details"${open ? ' open' : ''}><summary>How bundled Quad Views reproduces this</summary><p class="fine">${escape(pimax.sameLabelNote)}</p>`
      + `<dl class="pimax-values"><dt>Focus share of the view</dt><dd>${share(c.width)} × ${share(c.height)}</dd><dt>Focus pixel density</dt><dd>${num(c.focusScale)}×</dd><dt>Peripheral factor</dt><dd>${num(c.peripheralScale)}</dd><dt>Edge blending</dt><dd>${num(e.blend)}</dd>${pixels}</dl>`
      + (r ? `<p class="fine">${r.width} × ${r.height} is the per-eye resolution the headset runtime recommended in DCS's last session; sizes are rounded to an even pixel count as Pimax's runtime does.</p>` : '')
      + `${notes}<p class="fine">${escape(pimax.explanation)}</p></details>`;
  }
  box.innerHTML = `<div class="pimax-head"><div><b>Pimax Play · ${escape(pimax.mode)} mode</b><small title="${escape(pimax.path)}">Saved ${new Date(pimax.modified).toLocaleString('en-GB')}</small></div>${reread}</div>${controls}${mode === 'bundled' ? '<p class="fine">Focus area = the percentage shown in Pimax Play.</p>' : ''}${warnings}${how}`;
}
// ---- Saved DLSS 5 runtime -----------------------------------------------------------------------------------------
/** Verifies the file and keeps it as the app's copy; the draft is not changed. */
async function saveRuntime(file) {
  const saved = await request('rememberRuntime',{path:file});
  if (state) { state.savedRuntime = saved.savedRuntime; state.retainedRuntime = saved.retainedRuntime; }
  return saved;
}
async function forgetRuntime() {
  if (busy) return; setBusy(true);
  try { const result = await request('forgetRuntime'); state.savedRuntime = result.savedRuntime; state.retainedRuntime = result.retainedRuntime; change(); $('status').textContent = result.message; }
  catch (error) { showError(error); } finally { setBusy(false); updateControls(); }
}
function renderSavedRuntime() {
  const box = $('savedRuntime'), s = state?.savedRuntime, retained = state?.retainedRuntime, own = profile.neuralRuntimePath;
  $('input-neuralRuntimePath').placeholder = s ? 'Saved copy · version '+(s.version || 'unknown') : 'nvngx_dlssnr.dll';
  // With a saved copy, Change… replaces Browse.
  $('browse-neuralRuntimePath').hidden = Boolean(s);
  box.hidden = !s && !retained; if (box.hidden) { box.innerHTML = ''; return; }
  const kept = retained ? `<p class="fine">The forgotten copy stays at ${escape(retained)} because the applied profile uses it. It is deleted once that profile is no longer applied.</p>` : '';
  if (!s) { box.innerHTML = kept; return; }
  box.innerHTML = `<div class="saved-runtime-head"><div><b>Saved copy · version ${escape(s.version || 'unknown')} · from ${escape(s.originalPath)}</b><small>Kept at ${escape(s.path)} · ${s.modified ? 'NVIDIA\'s file changed by a compatibility patch, not signed by NVIDIA' : 'NVIDIA signature verified'} · saved ${new Date(s.savedAt).toLocaleString('en-GB')}${s.originalExists ? '' : ' · the original file is gone, the copy is still used'}</small></div><div class="saved-runtime-actions"><button type="button" class="button secondary" id="changeRuntime"${busy ? ' disabled' : ''}>Change…</button><button type="button" class="button quiet" id="forgetRuntime"${busy ? ' disabled' : ''}>Forget</button></div></div>`
    + (own ? '<p class="fine">This profile uses the file entered above. Clear the field to use the saved copy.</p>' : '') + kept;
}
document.addEventListener('click', e => {
  if (busy) return;
  if (e.target.closest('#changeRuntime')) $('browse-neuralRuntimePath').click();
  else if (e.target.closest('#forgetRuntime')) forgetRuntime();
});
// ---- DCS engine optimizations ----------------------------------------------------------------------------------
let engineTimer, engineSequence = 0, engineStatus = null;
/** The module's own log, read again every 5 s while the page is open (a test suite reports its end there). */
async function refreshEngine() {
  clearTimeout(engineTimer);
  const mine = ++engineSequence;
  try { const result = await request('engineStatus'); if (mine === engineSequence) { engineStatus = result.status; renderEngine(); } }
  catch (error) { if (mine === engineSequence) $('engineStatus').innerHTML = `<p class="fine">${escape(error.message)}</p>`; }
  if (page === 'engine' && mine === engineSequence) engineTimer = setTimeout(refreshEngine, 5000);
}
const ENGINE_STATE = {'not-installed':'Not installed', installed:'Installed', loaded:'Loaded', failed:'Load failed'};
function renderEngine() {
  const s = engineStatus, box = $('engineStatus'); if (!box) return;
  if (!s) { box.innerHTML = '<p class="fine">Select the DCS settings file on the DCS &amp; headset page first.</p>'; return; }
  const report = s.lastReport ? `<dt>Last test report</dt><dd>${escape(new Date(s.lastReportAt).toLocaleString('en-GB'))}</dd>` : '';
  // Depth boost or Visual realism alone installs the module too, with none of the optimizations (its Perception-only mode).
  const moduleUsed = Boolean(profile && (profile.engineOptimizations || usesDepthBoost(profile) || usesVisualRealism(profile)));
  const pending = profile && moduleUsed !== (s.state !== 'not-installed') ? `<p class="fine">${moduleUsed ? 'Launch DCS installs it with this profile.' : 'Launch DCS removes it with this profile.'}</p>` : '';
  const users = profile ? [usesDepthBoost(profile) ? 'Depth boost' : null, usesVisualRealism(profile) ? 'Visual realism' : null].filter(Boolean).join(' and ') : '';
  const perceptionOnly = profile && !profile.engineOptimizations && users ? `<p class="fine">Used by ${users} only: none of the optimizations run, and there is no test suite.</p>` : '';
  const devNote = profile?.engineOptimizations && profile.engineDevMode ? '<p class="pimax-note warn">Developer mode is on: DCS loads the module and its settings from the developer paths, not the ones this release installs. Turn it off on the left for normal flying.</p>' : '';
  box.innerHTML = devNote + `<div class="engine-state ${escape(s.state)}"><b>${escape(ENGINE_STATE[s.state] || s.state)}</b><span>${escape(s.summary)}</span></div>${pending}${perceptionOnly}`
    + s.warnings.map(w => `<p class="pimax-note warn">${escape(w)}</p>`).join('')
    + (s.suiteRunning ? '<p class="pimax-note">The test suite is running in DCS. Keep the view still (in VR, keep the headset on); it takes about 7 minutes.</p>' : '')
    + `<dl class="pimax-values">${report}</dl>`;
  $('engineSuite').disabled = busy || s.state !== 'loaded' || s.suiteRunning || !profile?.engineOptimizations;
  $('engineReport').disabled = busy || !s.lastReport;
  $('engineLog').disabled = busy || !s.logPath;
  renderSuiteNote();
}
/** Above the test suite's buttons: what makes Run test suite available. */
function renderSuiteNote() {
  const note = $('engineSuiteNote'); if (!note || !profile) return;
  const text = !profile.engineOptimizations ? 'Turn on Engine Optimizations to measure them in flight.' : engineStatus?.state !== 'loaded' ? 'Available while DCS runs with Engine Optimizations on.' : '';
  note.textContent = text; note.hidden = !text;
}
async function engineAction(action, data) {
  if (busy) return;
  try { const result = await request(action, data); if (result?.message) $('status').textContent = result.message; refreshEngine(); }
  catch (error) { showError(error); }
}
// ---- Pupil shift -------------------------------------------------------------------------------------------------
// <uses-rules>
/** The layer is installed only for a VR flight with eye-tracked Quad Views (VrProfile.UsesPupilShift, UsesDepthBoost,
 *  UsesPerceptionLayer: it carries both). */
const eyeTrackedVr = p => Boolean(!p.desktop && p.quadViews !== 'None' && p.gaze === 'EyeTracked');
const usesPupilShift = p => Boolean(p.pupilShift && eyeTrackedVr(p));
const usesDepthBoost = p => Boolean(p.depthBoost && eyeTrackedVr(p));
/** The strength of the selected Depth boost mode (VrProfile.DepthBoostActiveMax). */
const depthMax = p => p.depthBoostMode === 'Enhanced' ? p.depthBoostMax : p.depthBoostRealisticMax;
/** The strength the layer reaches with an eye distance of mm (1 when it is unknown): with the dual camera the world's
 *  eye distance is limited only by the selected strength, the cockpit keeps the real one. */
const depthMaxAt = (p, mm) => mm > 1 ? depthMax(p) : 1;
const usesPerceptionLayer = p => usesPupilShift(p) || usesDepthBoost(p) || usesGazePublish(p) || usesPeripheryDlssPreset(p);
/** The Periphery DLSS preset runs (VrProfile.UsesPeripheryDlssPreset): a VR flight with Quad Views-Foveated and Engine
 *  Optimizations. The pupil shift layer then hides the driver's forced preset from DLSS, so it is installed for it too. */
const usesPeripheryDlssPreset = p => Boolean(['E','F','K','M'].includes(p.peripheryDlssPreset) && !p.desktop && p.quadViews === 'QuadViewsFoveated' && p.engineOptimizations);
// ---- Visual realism (VrProfile.UsesColorMode, UsesGazeDof, UsesGVision, UsesVisualRealism) ---------------------------
/** The color mode key is set: it can switch to any mode in flight, so both modes' values are in use. */
const usesColorKey = p => Boolean(!p.desktop && parseKey(p.colorModeKey ?? 'Off')?.[0]);
const fixOn = p => !p.desktop && (p.colorMode === 'Fix' || usesColorKey(p));
const enhancedOn = p => !p.desktop && (p.colorMode === 'Enhanced' || usesColorKey(p));
const colorOn = p => !p.desktop && (p.colorMode !== 'Off' || usesColorKey(p));
/** Gaze DoF needs the gaze, which the pupil shift layer publishes from the eye-tracked Quad Views focus. */
const usesGazeDof = p => Boolean(p.gazeDof && eyeTrackedVr(p));
const gVisionOn = p => Boolean(!p.desktop && p.gVision);
const usesVisualRealism = p => colorOn(p) || usesGazeDof(p) || gVisionOn(p);
/** The layer publishes the gaze for the engine module (VrProfile.UsesGazePublish): eye-focus blur, G vision following the eyes. */
const usesGazePublish = p => usesGazeDof(p) || Boolean(gVisionOn(p) && p.gVisionUseGaze && eyeTrackedVr(p));
// </uses-rules>
/** What the profile still needs for a gaze, with a link to the setting that gives it. */
function renderPupilNeeds() {
  const ipd = $('pupilIpd');
  if (ipd && profile?.eyeDistance && profile.eyeDistance !== 'DcsCustom') { const set = profile.eyeDistance === 'Manual'; ipd.hidden = !(profile.pupilShift || profile.depthBoost); ipd.textContent = set ? `DCS renders with an eye distance of ${profile.eyeDistanceMm} mm (Set here)${profile.depthBoost ? `; Depth boost renders the world with up to ×${Number(depthMaxAt(profile, profile.eyeDistanceMm)).toFixed(2)}` : ''}. DCS's own custom IPD is off while this profile is applied.` : `DCS renders with the headset's own eye distance${profile.depthBoost ? '; Depth boost raises it while you look far' : ''}.${state?.dcsCustomIpd ? ` DCS's custom IPD (${state.dcsCustomIpd} mm) is off while this profile is applied; Back to stock DCS turns it on again.` : ''}`; }
  else if (ipd) { const mm = state?.dcsCustomIpd; ipd.hidden = !mm || !(profile?.pupilShift || profile?.depthBoost); ipd.textContent = mm ? `DCS renders with your custom IPD of ${mm} mm, but DCS Control sets it instead of DCS: otherwise DCS would undo the pupil's sideways move${profile?.depthBoost ? ' and Depth boost' : ''}.${profile?.depthBoost ? ` Depth boost renders the world with ×${Number(depthMaxAt(profile, mm)).toFixed(2)}: ${(mm * depthMaxAt(profile, mm)).toFixed(1)} mm.` : ''} DCS's own option is off while this profile is applied; Back to stock DCS turns it on again.` : ''; }
  if (!profile) return;
  showNeeds('pupilNeeds', gazeNeed(profile,'pupilFix'), profile.pupilShift);
  showNeeds('depthNeeds', gazeNeed(profile,'depthFix'), profile.depthBoost);
  const noQuad = !quad(profile), noEngine = !profile.engineOptimizations;
  showNeeds('farDepthNeeds', profile.desktop || (!noQuad && !noEngine) ? null : [noQuad && noEngine ? 'Needs the bundled Quad Views and Engine Optimizations.' : noQuad ? 'Needs the bundled Quad Views.' : 'Needs Engine Optimizations.',
    ...(noQuad ? [['farDepthFix','quadViews','Choose provider']] : []), ...(noEngine ? [['farDepthEngineFix','engineOptimizations','Turn on Engine Optimizations']] : [])], profile.farDepth);
  // How to check each feature in flight, with the key it has now.
  const key = path => { const k = parseKey(profile[path] ?? HOTKEYS[path].fallback); return k && k[0] ? keyLabel(k) : 'no key set'; };
  const lines = [['Pupil shift','pupilShiftToggleKey','look at a switch and move only your eyes left and right; the cockpit stays still. It does not fix lens blur or edge distortion.'],
    ['Depth boost','depthBoostToggleKey','look at a distant aircraft and press the key; the world gains depth, the cockpit does not.'],
    ['Far depth','farDepthToggleKey','look at distant terrain or ships and press the key; they gain depth, the cockpit does not.']];
  const check = $('perceptionCheck'), html = lines.map(([name,path,text]) => `<li><b>${name} (${escape(key(path))}):</b> ${text}</li>`).join('');
  if (check && check.innerHTML !== html) check.innerHTML = html;
}
/** Night vision and glare belong to the Enhanced color mode; Eye-focus blur needs the gaze, as Pupil shift does. */
function renderVisualNeeds() {
  if (!profile) return;
  showNeeds('enhancedNeeds', profile.desktop || enhancedOn(profile) ? null : ['Part of the Enhanced color mode.',['enhancedFix','colorMode','Set Color mode']], false);
  showNeeds('gazeDofNeeds', gazeNeed(profile,'gazeDofFix'), profile.gazeDof);
}
/** Periphery stability is a pass of the bundled Quad Views (VrProfile.UsesPeripheryStability). */
function renderStabilityNeeds() {
  const box = $('stabilityNeeds'); if (!box || !profile) return;
  const need = profile.peripheryStability && !profile.desktop && !quad(profile);
  box.hidden = !need;
  box.innerHTML = need ? `<p>Periphery stability runs in the bundled Quad Views only. With ${profile.quadViews === 'None' ? 'Quad Views off' : profile.quadViews === 'PimaxNative' ? 'Pimax native Quad Views' : 'an alternative provider'} it does not run.</p><button class="button secondary" id="stabilityFix">Choose the Quad Views provider</button>` : '';
  if (need) $('stabilityFix').addEventListener('click',() => revealSetting(fields.find(f => f.path === 'quadViews')));
}
let pupilTimer, pupilSequence = 0, pupilStatus = null;
/** The layer's log, read again every 5 s while the page is open (it writes once a second in flight). */
async function refreshPupil() {
  clearTimeout(pupilTimer);
  const mine = ++pupilSequence;
  try { const result = await request('pupilStatus'); if (mine === pupilSequence) { pupilStatus = result.status; renderPupil(); } }
  catch (error) { if (mine === pupilSequence) $('pupilStatus').innerHTML = `<p class="fine">${escape(error.message)}</p>`; }
  if (page === 'pupil' && mine === pupilSequence) pupilTimer = setTimeout(refreshPupil, 5000);
}
const PUPIL_STATE = {'not-installed':['Not installed',''], waiting:['Ready: starts with DCS',''], working:['Working','loaded'], 'no-gaze':['No gaze','failed'], off:['Switched off',''], 'not-restored':['Check the log','failed'], disabled:['Off','']};
const DEPTH_STATE = {disabled:['Not running',''], waiting:['Ready: starts with DCS',''], working:['Working','loaded'], probing:['Waiting for both distances',''], 'no-depth':['No depth from the gaze','failed'], 'no-scene':['Normal depth: no depth from DCS yet',''], 'no-gaze':['No gaze','failed'], off:['Switched off','']};
/** Depth boost's part of the layer's log: the current scale and what it did this flight. */
function depthStatusHtml(d) {
  if (!d) return '';
  const [label, tone] = DEPTH_STATE[d.state] || [d.state,''];
  const l = d.last, distance = m => m < 0 ? '—' : m >= 999 ? (l && l.source === 'depth' ? 'far (999 m or more)' : 'far (no convergence)') : m.toFixed(m < 10 ? 2 : 0)+' m';
  const rows = d.seconds ? [
    ['Scale now / highest this flight', l ? `×${l.scale.toFixed(2)} / ×${d.sessionMaxScale.toFixed(2)}` : '—'],
    ['Looking at', l ? `${l.fixation === 'near' ? 'the cockpit' : l.fixation === 'far' ? 'outside' : 'not known yet'} · ${distance(l.distanceM)}` : '—'],
    ['Distance measured from', l ? ({depth:'the scene', vergence:'your eyes\' vergence'}[l.source] || 'nothing yet') + (l.mode ? ` · ${l.mode === 'enhanced' ? 'Enhanced' : 'Realistic'}` : '') : '—'],
    ['Seconds boosted', `${d.secondsBoosted.toFixed(0)} of ${d.seconds}`],
    ['Seconds looking at the cockpit / outside', `${d.secondsNear.toFixed(0)} / ${d.secondsFar.toFixed(0)}`],
    ['Eye distance: normal / now', l ? `${l.baseIpdMm.toFixed(1)} / ${l.renderIpdMm.toFixed(1)} mm` : '—'],
    ['In-flight switch', d.hotkey ? keyLabel(parseKey(d.hotkey)) : '—'],
  ] : [];
  const report = rows.filter(([k]) => k.startsWith('Eye distance'));
  return `<h3 class="boost-section">Depth boost</h3><div class="engine-state ${tone}"><b>${escape(label)}</b><span>${escape(d.summary)}</span></div>`
    + statusRows(rows.filter(r => !report.includes(r)), report);
}
/** Rows for the status, and rows only a report needs in a closed fold. */
const statusRows = (rows, report) => (rows.length ? `<dl class="pimax-values">${rows.map(([k,v]) => `<dt>${escape(k)}</dt><dd>${escape(v)}</dd>`).join('')}</dl>` : '')
  + (report.length ? `<details class="report-details"><summary>Details for a report</summary><dl class="pimax-values">${report.map(([k,v]) => `<dt>${escape(k)}</dt><dd>${escape(v)}</dd>`).join('')}</dl></details>` : '');
const degrees = r => r ? `${r.min > 0 ? '+' : ''}${r.min.toFixed(0)}° to ${r.max > 0 ? '+' : ''}${r.max.toFixed(0)}°` : '—';
function renderPupil() {
  const s = pupilStatus, box = $('pupilStatus'); if (!box) return;
  if (!s) { box.innerHTML = '<p class="fine">Reading the status…</p>'; $('pupilLog').disabled = true; return; }
  const installed = s.state !== 'not-installed';
  const pending = profile && usesPerceptionLayer(profile) !== installed ? `<p class="fine">${usesPerceptionLayer(profile) ? 'Launch DCS installs it with this profile.' : 'Launch DCS removes it with this profile.'}</p>` : '';
  const [label, tone] = PUPIL_STATE[s.state] || [s.state,''];
  const all = s.seconds ? [
    ['Gaze range, left–right', degrees(s.sessionGazeX)], ['Gaze range, up–down', degrees(s.sessionGazeY)],
    ['Largest pupil shift', s.sessionMaxShiftMm ? s.sessionMaxShiftMm.toFixed(1)+' mm' : '—'],
    ['Seconds with a gaze', `${s.secondsWithGaze} of ${s.seconds}`],
    ['Image placed as without Pupil shift', s.restoredViews == null ? '—' : s.restoredViews > 0 ? 'Yes' : 'No'],
    ['In-flight switch', s.hotkey ? keyLabel(parseKey(s.hotkey)) : '—'],
    ['Headset views turned outwards (cant)', s.last?.cantDeg != null ? s.last.cantDeg.toFixed(1)+'°, straight ahead taken between them' : '—'],
    ['Eye distance: headset / DCS renders', s.last?.runtimeIpdMm != null ? `${s.last.runtimeIpdMm.toFixed(1)} / ${s.last.renderedIpdMm.toFixed(1)} mm` : '—']
  ] : [];
  // The cant, the restored views and the eye distances matter for a report, not for flying.
  const report = all.filter(([k]) => k.startsWith('Image placed') || k.startsWith('Headset views') || k.startsWith('Eye distance')), rows = all.filter(r => !report.includes(r));
  // Open details stay open when the status is read again every 5 s.
  const open = [...box.querySelectorAll('details.report-details')].map(d => d.open);
  box.innerHTML = `<h3 class="boost-section">Pupil shift</h3><div class="engine-state ${tone}"><b>${escape(label)}</b><span>${escape(s.summary)}</span></div>${pending}`
    + s.warnings.map(w => `<p class="pimax-note warn">${escape(w)}</p>`).join('')
    + statusRows(rows, report)
    + depthStatusHtml(s.depth);
  box.querySelectorAll('details.report-details').forEach((d,i) => { d.open = Boolean(open[i]); });
  $('pupilLog').disabled = busy || !s.logPath;
}
// ---- Diagnostics ---------------------------------------------------------------------------------------------------
let dlssFocusSequence = 0;
const FOCUS_STATE = {applied:['Applied','loaded'], 'not-applied':['Not applied','failed'], 'no-dlss':['No DLSS frames','failed'], 'no-session':['No session','']};
/** Cheeky's log of the last DCS session: whether DLSS 5 reached the focus area, and if not why. */
async function refreshDlssFocus() {
  const mine = ++dlssFocusSequence, box = $('dlssFocus'); if (!box) return;
  try {
    const {report:r} = await request('dlssFocus'); if (mine !== dlssFocusSequence) return;
    const [label, tone] = FOCUS_STATE[r.state] || [r.state,''];
    const rows = [['DLSS 5 runtime',r.version],['Focus views identified',r.accepted ? String(r.accepted) : null],['Rejections',r.rejected ? String(r.rejected) : null],
      ['DCS ran DLSS at',r.dlssSize?.replace('x','×')],['Focus views',r.focusSize?.replace('x','×')]].filter(([,v]) => v);
    box.innerHTML = `<div class="engine-state ${tone}"><b>${escape(label)}</b><span>${escape(r.summary)}</span></div>`
      + (r.explanation ? `<p class="pimax-note${r.state === 'applied' ? '' : ' warn'}">${escape(r.explanation)}</p>` : '')
      + (rows.length ? `<dl class="pimax-values">${rows.map(([k,v]) => `<dt>${escape(k)}</dt><dd>${escape(v)}</dd>`).join('')}</dl>` : '')
      + (r.evidence.length ? `<details class="advanced"><summary>Log lines<small>From CheekyFoveatedDLSS-Standalone.log</small></summary><pre class="log-lines">${escape(r.evidence.join('\n'))}</pre></details>` : '');
  } catch (error) { if (mine === dlssFocusSequence) box.innerHTML = `<p class="fine">${escape(error.message)}</p>`; }
}
// ---- CPU Boost plan ---------------------------------------------------------------------------------------------
let boostTimer, boostSequence = 0;
function refreshBoostPlan(delay = 300) {
  clearTimeout(boostTimer);
  boostTimer = setTimeout(async () => {
    const mine = ++boostSequence;
    try { const result = await request('boostPlan',draft()); if (mine === boostSequence) renderBoostPlan(result); }
    catch (error) { if (mine === boostSequence) $('boostPlan').innerHTML = `<p class="fine">${escape(error.message)}</p>`; }
  }, delay);
}
const cores = list => { if (!list?.length) return 'None'; const out = []; let start = list[0], prev = list[0]; for (const n of [...list.slice(1), null]) { if (n === prev + 1) { prev = n; continue; } out.push(start === prev ? String(start) : start+'–'+prev); start = prev = n; } return 'CPU '+out.join(', '); };
const megabytes = bytes => bytes == null ? '—' : (bytes >= 1073741824 ? (bytes/1073741824).toFixed(1)+' GB' : Math.round(bytes/1048576)+' MB');
function renderBoostPlan({plan:b, dcsLog}) {
  // Each section: a title, one summary line, and what it reads on this PC folded under "Details for this PC".
  const section = (title,summary,details,id = '') => `<h3 class="boost-section"${id ? ` id="${id}"` : ''}>${title}</h3><p class="boost-summary">${summary}</p>` + (details ? `<details class="boost-details"><summary>Details for this PC</summary>${details}</details>` : '');
  const plural = (n,one,many) => n+' '+(n === 1 ? one : many);
  // Free VRAM before flight: each listed program, running processes and their video memory now (approximate).
  const apps = b.freeVram || [], running = apps.filter(a => a.processes);
  const vram = section('Free VRAM before flight',!apps.length ? 'The list is empty: nothing would be closed.' : (profile.freeVram ? 'Closes ' : 'Off. Now it would close ') + (running.length ? `${plural(running.length,'running program','running programs')}, about ${megabytes(b.freeVramBytes)} of video memory.` : 'nothing: none of the listed programs is running.'), apps.length
    ? `<table class="boost-table vram-table"><thead><tr><th>Program</th><th>Running</th><th>VRAM (approx.)</th></tr></thead><tbody>${apps.map(a => `<tr><td><b>${escape(a.name)}</b>${a.skipped ? '<small>Protected processes skipped</small>' : a.needsAdministrator ? '<small>Needs admin rights to close</small>' : ''}</td><td>${a.processes ? a.processes+(a.processes === 1 ? ' process' : ' processes') : 'No'}</td><td>${a.processes ? megabytes(a.dedicatedBytes) : '—'}</td></tr>`).join('')}</tbody><tfoot><tr><td><b>Total to free</b></td><td></td><td id="vramTotal"><b>${megabytes(b.freeVramBytes)}</b></td></tr></tfoot></table>`
      + '<p class="fine">Per-process GPU counters; memory shared between programs can be counted twice, so the total is approximate.</p>'
    : '<p class="fine">The list is empty: nothing would be closed.</p>','vramPlanTitle');
  const m = b.monitor, mode = x => `${x.width}×${x.height} at ${x.refresh} Hz`, vrOnly = 'Not with Optimizations only.';
  const monitor = section('Monitor while flying',profile.desktop ? vrOnly : !profile.lowerMonitor ? 'Off. The monitor keeps its mode.' : m ? `${escape(mode(m.current))} → ${escape(mode(m.flight))} while DCS runs.` : 'No display could be read.', m
    ? `<dl class="boost-cores" id="monitorPlan"><dt>Display</dt><dd>${escape(m.name)}</dd><dt>Mode</dt><dd>${escape(mode(m.current))} → ${escape(mode(m.flight))}</dd></dl><p class="fine">${escape(m.note)}</p>` : '');
  const small = section('Small DCS window in VR',profile.desktop ? vrOnly : profile.smallDcsWindow ? 'The DCS window on the monitor becomes 1280×720, windowed.' : 'Off. The DCS window keeps your size.',
    `<p class="fine">Set in DCS's settings: ${escape(b.smallWindow || '')}. Back to stock DCS puts your values back.</p>`);
  const services = b.tobiiServices || [];
  const tobii = section('Desktop Tobii eye tracker',profile.desktop ? vrOnly : !services.length ? 'No desktop Tobii eye tracker on this PC: nothing to pause.' : profile.pauseTobiiDesktop ? `${plural(services.length,'Tobii service','Tobii services')} paused while DCS runs.` : 'Off. A desktop Tobii tracker keeps running.', services.length
    ? `<p class="fine">Paused while DCS runs, started again when it exits: ${services.map(s => '<b>'+escape(s)+'</b>').join(', ')}. The eye tracking services of the headset are not touched.</p>` : '');
  // CPU Boost: priority and cores for DCS, and how many headset services and apps move away, then the full plan.
  const rows = [['DCS main and render threads',b.renderCores],['VR runtime and headset services',b.vrRuntimeCores],['Background apps',b.backgroundCores]];
  const processes = b.processes.length ? `<table class="boost-table"><thead><tr><th>Process</th><th>Change</th><th>Running</th><th>Admin</th></tr></thead><tbody>${b.processes.map(p => `<tr><td><b>${escape(p.name)}</b><small>${escape(p.category)}</small></td><td>${escape(p.action)}</td><td>${p.running ? 'Yes' : 'No'}</td><td>${p.needsAdministrator ? (profile.boostElevated ? 'Yes · asked at launch' : 'Needed · left as is') : '—'}</td></tr>`).join('')}</tbody></table>` : '<p class="fine">No processes would change.</p>';
  const count = category => b.processes.filter(p => p.category === category).length, vr = count('VR runtime'), background = count('Background');
  const priority = {Normal:'Normal',AboveNormal:'Above normal',High:'High'}[profile.boostDcsPriority] || profile.boostDcsPriority;
  const moved = [vr ? plural(vr,'headset service','headset services') : '', background ? plural(background,'app','apps') : ''].filter(Boolean);
  const cpu = profile.cpuBoost
    ? section('CPU Boost',`DCS gets ${escape(priority)} priority and ${escape(cores(b.renderCores))}` + (moved.length ? `; ${moved.join(' and ')} move to other cores.` : '.'),
      `<dl class="boost-cores"><dt>Core ranking</dt><dd>${escape(b.sourceDescription)}</dd>${rows.map(([k,v]) => `<dt>${k}</dt><dd>${escape(cores(v))}</dd>`).join('')}<dt>DCS log</dt><dd>${escape(dcsLog)}</dd></dl>` + processes + (b.notes.length ? `<ul class="boost-notes">${b.notes.map(n => `<li>${escape(n)}</li>`).join('')}</ul>` : ''))
    : section('CPU Boost','Off. Turn on CPU Boost to see what it would change.','');
  // The helpers first, then CPU Boost.
  $('boostPlan').innerHTML = vram + monitor + small + tobii + cpu;
  fillDisplayModes(m);
}
/** The reason a greyed-out field gives (field()'s why) as [text, path to open], or null. */
function reasonOf(f) {
  if (!f.element.classList.contains('inactive') || !f.why) return null;
  const r = typeof f.why === 'function' ? f.why(profile) : f.why; return !r ? null : Array.isArray(r) ? r : [r];
}
function setReason(element, r) {
  const key = r ? r.join('|') : ''; if (element.dataset.reason === key) return; element.dataset.reason = key; element.hidden = !r;
  element.innerHTML = r ? escape(r[0]) + (r[1] ? ` <button type="button" class="link-button" data-reveal="${escape(r[1])}">Open</button>` : '') : '';
}
/** Says why settings are greyed out: once under the card heading when every setting of the card waits for the same thing,
 *  else under each greyed-out setting, once for a run of settings that wait for the same thing. */
function showReasons() {
  const cards = new Map();
  fields.forEach(f => { const c = f.element.closest('.card'); if (c) cards.set(c,[...(cards.get(c) || []),f]); });
  cards.forEach((list, c) => {
    const shown = list.filter(f => !f.element.hidden), reasons = shown.map(reasonOf);
    const whole = shown.length && reasons.every(r => r && r[0] === reasons[0][0]) ? reasons[0] : null;
    let head = c.querySelector(':scope > .card-heading .card-why');
    if (whole && !head && c.querySelector(':scope > .card-heading > div')) { head = document.createElement('p'); head.className = 'why card-why'; c.querySelector(':scope > .card-heading > div').append(head); }
    if (head) setReason(head, whole);
    let previous = null, parent = null;
    list.forEach(f => {
      const r = f.element.hidden || whole ? null : reasonOf(f), repeat = r && previous && previous[0] === r[0] && parent === f.element.parentElement;
      setReason(f.whyElement, repeat ? null : r); if (!f.element.hidden) { previous = r; parent = f.element.parentElement; }
    });
  });
}
function updateControls() {
  if (!profile) return;
  document.querySelectorAll('input[type=range]').forEach(paintRange);
  fields.forEach(f => { const inactive = Boolean(f.condition && !f.condition(profile)); f.element.classList.toggle('inactive',inactive); f.input.disabled = busy || inactive; if (f.range) f.range.disabled = busy || inactive; f.element.hidden = Boolean(f.hide && f.hide(profile)); });
  // A fold that follows a choice (the color mode's fine-tuning) is gone while all its settings are.
  document.querySelectorAll('details[data-hide]').forEach(d => { d.hidden = [...d.querySelectorAll('.setting')].every(e => e.hidden); });
  // With Optimizations only there is no headset panel; the In flight page keeps the keys that work on a monitor.
  $('panelControls').hidden = Boolean(profile.desktop);
  // Pimax native exists only on the Pimax route and cannot host the focus adapter DLSS needs.
  const native = $('input-quadViews').querySelector('[value=PimaxNative]'); native.disabled = !nativeAllowed(profile);
  native.textContent = 'Pimax native Quad Views' + (profile.runtime === 'SboysSteamVr' ? ' · Pimax route only' : native.disabled ? ' · not with DLSS 5' : '');
  // Segmented choices follow their select's value, its disabled state and option texts (Pimax native's just above).
  fields.forEach(syncSegment);
  // Foveated Super Resolution is a stereo option: its card is gone while Quad Views is on.
  $('foveatedDlssControls').hidden = $('stereoFocusControls').hidden = profile.quadViews !== 'None';
  // While the rate is read from the headset, say which value is used: the one read, or this one until one is read.
  { const f = fields.find(x => x.path === 'headsetRefreshHz'), hz = state?.headsetRefresh?.hz;
    $('help-headsetRefreshHz').textContent = !profile.refreshFromHeadset ? f.description : hz ? `Read from the headset: ${num(hz)} Hz.` : 'Not read yet: this value is used until the headset software reports one.'; }
  // Stereo Cheeky clamps these ratios to 0.2 or more: say which value is written when the field holds less.
  fields.filter(f => ['foveaWidth','foveaHeight','peripheralScale'].includes(f.path)).forEach(f => {
    const help = $('help-'+f.path), clamped = stereoCheeky(profile) && Number(profile[f.path]) < CHEEKY_MIN;
    help.textContent = f.description + (clamped ? ` Without Quad Views the minimum is ${CHEEKY_MIN}, so ${CHEEKY_MIN} is written.` : '');
    f.element.classList.toggle('clamped', clamped);
  });
  // Bundled Quad Views renders at most 90% of the view per axis: Horizontal or Vertical FOV 90% in Pimax Play's Quick units.
  fields.filter(f => ['pimaxHorizontalFov','pimaxVerticalFov'].includes(f.path)).forEach(f => {
    const value = f.map.get(profile), capped = quad(profile) && !pimaxSource(profile) && PIMAX.quickFromShare(profile[f.path === 'pimaxHorizontalFov' ? 'foveaWidth' : 'foveaHeight']) > 90 + 1e-9;
    $('help-'+f.path).textContent = f.description + (capped ? ` Bundled Quad Views renders a focus area of at most 90% of the view, so ${value}% renders as 90%.` : '');
    f.element.classList.toggle('clamped', capped);
  });
  const e = effectiveFovea(profile), owns = coverageOwned(profile);
  $('focusBox').style.width = e.w*100+'%'; $('focusBox').style.height = e.h*100+'%'; $('focusBox').hidden = profile.quadViews === 'None' && !owns;
  // Pimax Play's units for Quad Views; fractions of the view for stereo Cheeky without Quad Views.
  const pimaxFound = state?.pimax?.found;
  $('coverageDetail').textContent = quad(profile) ? (e.pimax ? `Pimax Play ${state.pimax.short}` : `Horizontal FOV ${whole(PIMAX.quickFromShare(profile.foveaWidth))}% · Vertical FOV ${whole(PIMAX.quickFromShare(profile.foveaHeight))}% · Center ${whole(PIMAX.centerFromDensity(profile.quadFocusScale))}% · Peripheral ${whole(PIMAX.peripheralPercent(profile.peripheralScale))}% (Pimax Quick units)`)
    : owns ? `${Math.round(e.w*100)}% width · ${Math.round(e.h*100)}% height · ${Math.round(e.periphery*100)}% peripheral scale` : profile.quadViews === 'PimaxNative' && pimaxFound ? `Pimax native · Pimax Play ${state.pimax.short}` : 'Coverage is controlled by the selected provider.';
  // Pixels DCS renders for the four views, against normal stereo at the same resolution (before DLSS; overlap counts twice).
  const pixels = quad(profile) ? e.periphery**2 + e.w*e.h*e.focus**2 : null;
  $('coverageNote').textContent = pixels !== null ? `DCS renders about ${Math.round(pixels*100)}% of the pixels of normal stereo rendering (before DLSS upscaling).` : owns ? '' : 'Pimax native and alternative providers set their coverage in their own tools.';
  renderPimax(); renderFeatures(); renderSavedRuntime();
  // One tile per feature, in pipeline order. Each opens the page that configures it.
  const on = featureState(profile);
  const tiles = [
    ['engine','ENGINE',on.engine,on.engine ? 'Engine Optimizations' : 'Off',featureSummary('engine',profile)],
    ['boost','CPU BOOST',on.boost,on.boost ? 'CPU Boost' : 'Off',featureSummary('boost',profile)],
    ['foveation','QUAD VIEWS',on.quad,profile.quadViews === 'None' ? 'Off' : profile.quadViews === 'PimaxNative' ? 'Pimax native' : 'Bundled Quad Views',featureSummary('quad',profile)],
    ['dlss','DLSS 5',on.dlss,profile.neuralRendering ? 'DLSS 5' : foveatedSr(profile) ? 'Foveated Super Resolution' : 'DLSS 5',featureSummary('dlss',profile)],
    ['framegen','FRAME GENERATION',on.framegen,profile.frameGen === 'Off' ? 'Off' : profile.frameGen === 'Nvidia' ? 'NVIDIA Optical Flow' : 'FidelityFX Optical Flow',featureSummary('framegen',profile)]
  ];
  // Two groups: the optimizations every pilot can use, then the VR features (not with Optimizations only).
  const names = {engine:'Engine Optimizations',boost:'CPU Boost',foveation:'Quad Views',dlss:'DLSS 5',framegen:'Frame generation'};
  // An active tile's title already names the provider, so its detail does not repeat it.
  const tile = ([target,label,active,title,detail]) => { if (!active) { title = names[target] || title; detail = String(detail).replace(/^Off · /,''); } else detail = String(detail).replace(/^(Bundled|Pimax native|NVIDIA|FidelityFX) · /,''); return `<button type="button" class="stage ${active ? 'on' : 'off'}" data-page="${target}" aria-label="${escape(title)}: ${active ? 'on' : 'off'}. Open settings"><span>${label}<em>${active ? 'ON' : 'OFF'}</em></span><b>${escape(title)}</b><small>${escape(detail)}</small></button>`; };
  const group = (label, list) => `<div class="stage-group"><div class="stage-group-label">${label}</div><div class="stage-row${list.length === 2 ? ' two' : ''}">${list.map(tile).join('')}</div></div>`;
  const everyone = tiles.filter(([target]) => !VR_PAGES.includes(target)), vr = tiles.filter(([target]) => VR_PAGES.includes(target));
  // Then, in VR, what the checklist does not show (as the "Also on" line under it): Perception, Visual realism, the headset panel.
  const look = [['pupil','EYES &amp; DEPTH','Perception',g => g.section === 'perception','Depth boost, Far depth, Pupil shift'],['visual','COLOR, NIGHT &amp; G','Visual realism',g => g.section === 'visual','Color, night vision, G vision'],['osd','HEADSET PANEL','In flight',g => g.page === 'osd','Status panel in the headset']].map(([target,label,name,test,offer]) => {
    const list = FEATURE_GUIDE.filter(g => test(g) && g.on(profile)).map(g => g.name === 'Color modes' ? 'Color: '+profile.colorMode : g.name);
    return [target,label,list.length > 0,name,list.length ? list.join(' · ') : offer];
  });
  const extra = `<div class="stage-group extra"><div class="stage-group-label">Perception &amp; look</div><div class="stage-row">${look.map(tile).join('')}</div></div>`;
  $('pipeline').innerHTML = group('For every pilot · monitor or any headset', everyone) + (profile.desktop ? '' : group('VR · ' + (profile.runtime === 'SboysSteamVr' ? 'Sboys' : 'Pimax'), vr) + extra);
  // What DLSS 5 still needs, on its own page; everything else to fix is listed once, in Checks.
  const notes = featureNotes(profile);
  $('dlssMissing').innerHTML = notes.dlss.filter(n => n[0] === 'needs').map(() => `<div class="needs"><span>DLSS 5 needs your nvngx_dlssnr.dll runtime file.</span></div>`).join('');
  // Inside a job that forbids breakaway the DCS launcher cannot start the game (LaunchSafety.CurrentJob).
  const launcher = fields.find(f => f.path === 'keepDcsLauncher'), blocked = Boolean(state?.launcherBlocked);
  $('help-keepDcsLauncher').textContent = launcher.description + (blocked ? ' Not available now: this copy of DCS Control was started by another program inside a Windows job, where the launcher cannot start the game (error 5). Open DCS Control from the Start menu or Explorer to use it.'
    : state?.launcherUnknown ? ' Unknown here: this copy runs inside a Windows job whose limits could not be read. If DCS does not open after the launcher, open DCS Control from the Start menu or Explorer.' : '');
  launcher.element.classList.toggle('clamped', blocked);
  if (blocked && !profile.keepDcsLauncher) { launcher.input.disabled = true; launcher.element.classList.add('inactive'); }
  showReasons();
  fields.filter(f => f.type === 'hotkey').forEach(f => { if (!f.input.classList.contains('capturing')) showHotkey(f); });
  renderDashboard();
  const live = state?.activeRoute?.route;
  document.querySelectorAll('#routeSwitch [data-route]').forEach(button => {
    const selected = profile.desktop ? button.dataset.route === 'Desktop' : button.dataset.route === profile.runtime;
    button.classList.toggle('selected', selected); button.setAttribute('aria-checked', String(selected));
    button.classList.toggle('live', live === button.dataset.route); button.disabled = busy;
  });
  $('routeLive').textContent = profile.desktop ? 'Another headset or a monitor: DCS runs as you set it up, with Engine Optimizations, CPU Boost and Free VRAM.' : state?.activeRoute?.summary || '';
  $('routeLive').classList.toggle('mismatch', !profile.desktop && Boolean(live) && live !== profile.runtime);
  // The VR feature pages are hidden without a headset; an open one goes back to the overview.
  VR_PAGES.forEach(id => $('nav-'+id).hidden = Boolean(profile.desktop)); $('railGroup-foveation').hidden = Boolean(profile.desktop); $('headsetPanelAlways').hidden = Boolean(profile.desktop) || profile.frameGen === 'Off';
  renderAlsoOn(); $('dlssFocusCard').hidden = Boolean(profile.desktop) || !profile.neuralRendering;
  if (profile.desktop && VR_PAGES.includes(page)) navigate('overview');
  // The Sboys driver is only needed on the Sboys route.
  $('sboysCard').hidden = profile.runtime !== 'SboysSteamVr' || Boolean(profile.desktop);
  $('headsetControls').hidden = Boolean(profile.desktop);
  renderHeadsetSoftware(); renderSuiteNote();
  renderCadence();
  // A fold whose settings are all greyed out shows its value greyed out too.
  foldValues.forEach(({box,value}) => { box.querySelector(':scope > summary > .fold-value').textContent = value(profile);
    const shownSettings = [...box.querySelectorAll('.setting')].filter(e => !e.hidden); box.classList.toggle('idle', shownSettings.length > 0 && shownSettings.every(e => e.classList.contains('inactive'))); });
  // A folded program list can be opened and read while its setting is greyed out, but not while a command runs.
  fields.forEach(f => { if (f.listSummary) f.listSummary.disabled = busy; });
  updateWorkflow(); updatePanelScroll();
}
/** DCS & headset: the headset software of this profile (with Pimax Play's version) and what runs on this PC now. */
function renderHeadsetSoftware() {
  const version = state?.inventory?.pimaxVersion, summary = state?.activeRoute?.summary;
  const software = profile.runtime === 'SboysSteamVr' ? 'SteamVR' : 'Pimax Play' + (version ? ' '+version : '');
  $('headsetActive').innerHTML = `<b>${escape(software)}</b>${summary ? `<span>${escape(summary)}</span>` : ''}`;
}
/** Under the checklist: the Perception, Visual realism and headset panel features that apply at launch, each opening its page. */
function renderAlsoOn() {
  const on = FEATURE_GUIDE.filter(g => (['perception','visual'].includes(g.section) || g.page === 'osd') && g.on(profile));
  const name = g => g.name === 'Color modes' ? 'Color '+profile.colorMode : g.name;
  $('alsoOn').innerHTML = on.length ? 'Also on: '+on.map(g => `<button type="button" class="link-button" data-open-page="${g.page}" title="Open ${escape(g.name)} settings">${escape(name(g))}</button>`).join(', ') : '';
  $('alsoOn').hidden = !on.length;
}
/** Shadow above the workflow footer while part of the right panel is scrolled out of view below. */
function updatePanelScroll() {
  const top = document.querySelector('.panel-top');
  document.querySelector('.workflow').classList.toggle('more-above', top.scrollHeight - top.clientHeight - top.scrollTop > 1);
}
function renderCadence() {
  // multiplier: the most displayed frames per rendered frame; target: what DCS should render (FramePacing.CapDivider),
  // the full refresh rate with frame generation only when needed.
  const fg = profile.frameGen !== 'Off', adaptive = usesAdaptiveCadence(profile), multiplier = fg ? (profile.frameGenFactor === 3 ? 3 : 2) : 1, hz = refreshHz(profile), target = hz / capDivider(profile);
  const detected = Number(state?.inventory?.dcsSettings?.['graphics.maxFPS']);
  const cap = profile.fpsLimit === 'Preserve' ? (Number.isFinite(detected) && detected > 0 ? detected : null) : profile.fpsLimit === 'RuntimeHeadroom' ? 300 : profile.fpsLimit === 'MatchRefresh' ? Math.max(30, target) : profile.renderedFpsCap;
  const n = value => Number(value.toFixed(3)).toLocaleString('en-US');
  const metrics = [
    ['DCS NEEDS TO RENDER',n(target)+' FPS',adaptive ? 'The full refresh rate; generated frames only while DCS falls behind.' : fg ? (profile.frameGenFactor === 0 ? 'Half the refresh rate; a third while DCS falls behind.' : multiplier === 3 ? 'A third of the refresh rate.' : 'Half the refresh rate.') : 'The full refresh rate; frame generation is off.'],
    ['DCS FRAME LIMIT',cap === null ? 'Not set' : n(cap)+' FPS',profile.fpsLimit === 'Preserve' ? (cap === null ? 'DCS has no limit set; left as it is.' : 'DCS\'s own limit, left as it is.') : 'Set in DCS at launch.'],
    ['HEADSET',n(hz)+' Hz' + (profile.refreshFromHeadset && state?.headsetRefresh ? ' · '+escape(state.headsetRefresh.source) : ''),fg ? `Up to ${n(cap === null ? hz : Math.min(hz,cap*multiplier))} FPS with generated frames.` : 'Set this refresh rate in Pimax Play or SteamVR.']
  ];
  $('cadenceMetrics').innerHTML = metrics.map(b => `<div class="budget"><span>${b[0]}</span><strong>${b[1]}</strong><p>${b[2]}</p></div>`).join('');
  const warnings = [];
  if (cap !== null && cap + .01 < target && adaptive) warnings.push(`Frame generation only when needed works when DCS's frame limit is the refresh rate (${n(hz)} FPS). With ${n(cap)} FPS, frames are always generated. Choose Match refresh.`);
  else if (cap !== null && cap + .01 < target) warnings.push(`The frame limit (${n(cap)} FPS) is below what DCS needs to render (${n(target)} FPS). Raise the limit or choose a lower headset refresh rate.`);
  if (state?.inventory?.autoexecPath && (state.inventory.autoexecMaxFps !== null || state.inventory.autoexecPacingUnknown)) warnings.push('DCS\'s autoexec.cfg sets its own frame limit'+(state.inventory.autoexecMaxFps ? ' (max_fps = '+state.inventory.autoexecMaxFps+')' : '')+'. DCS Control leaves it as it is; make sure it does not hold DCS back.');
  $('cadenceWarnings').innerHTML = warnings.map(w => `<div class="issue Warning">${escape(w)}</div>`).join('');
}
// "Applied" means the draft has the applied profile.json's content; id and name only follow the features.
/** Values the backend computes from the other fields (VrProfile's read-only properties): the page never updates them, so
 *  they stay out of the comparison, like ControlService.SameDraft. */
const derivedField = k => /^uses[A-Z]/.test(k) || ['quadSaccadeLeadMs','depthBoostActiveMax','neuralEdgeFeather','boostHelperElevated'].includes(k);
const canonical = value => Array.isArray(value) ? value.map(canonical) : value && typeof value === 'object'
  ? Object.fromEntries(Object.keys(value).filter(k => k !== 'id' && k !== 'name' && !derivedField(k)).sort().map(k => [k, canonical(value[k])])) : value;
/** The same file, as ControlService.SamePaths compares them: an empty side (detected automatically, or a profile that
 *  leaves options.lua untouched) matches anything. */
const normPath = p => String(p || '').trim().replace(/\//g,'\\').replace(/\\+$/,'').toLowerCase();
const samePath = (a, b) => !normPath(a) || !normPath(b) || normPath(a) === normPath(b);
function matchesApplied() {
  return Boolean(profile && state?.launchReady && state.appliedDraft && JSON.stringify(canonical(profile)) === JSON.stringify(canonical(state.appliedDraft))
    && samePath(dcs, state.appliedDcs) && samePath(options, state.appliedOptions));
}
/** The one status line: what Launch DCS will do now. */
function launchState() {
  if (invalid.size) return 'invalid';
  if (state?.dcsRunning) return 'running';
  return matchesApplied() ? 'ready' : 'changes';
}
function updateWorkflow() {
  const hasInvalid = invalid.size > 0, mode = launchState(), applied = state?.appliedName || state?.appliedProfile;
  const blocked = busy || !profile || !state || ['invalid','running'].includes(mode);
  $('launch').disabled = blocked;
  // Apply without launching has nothing to do while the draft is the applied profile, unless Pimax Play changed.
  $('apply').disabled = blocked || (mode === 'ready' && !state.foveaChanged);
  $('reviewPlan').disabled = $('reviewPlanHere').disabled = busy || hasInvalid || !profile;
  $('openRecovery').hidden = !state?.originals?.count; $('openRecovery').disabled = busy || mode === 'running';
  $('saveProfile').disabled = busy || hasInvalid;
  const error = $('statusDot').classList.contains('error');
  $('statusTitle').textContent = busy ? 'Working' : hasInvalid ? STATE_WORDS.invalid : error ? 'Needs attention' : STATE_WORDS[mode];
  if (hasInvalid) $('status').textContent = 'Correct the marked values before launching or saving.';
  else if (!busy && !error && mode === 'running') $('status').textContent = 'Close DCS to apply changes or go back to stock DCS.';
  $('statusDot').classList.toggle('ready', !busy && !error && mode === 'ready');
  $('launch').title = mode === 'running' ? 'DCS is already running'
    : mode === 'ready' ? 'Launch DCS with '+profile.name+(state.foveaChanged ? ' (Pimax Play\'s new focus values are written first)' : '')
    : applied ? 'Apply '+(profile?.name || 'this profile')+' over '+applied+', then launch DCS' : 'Back up the original files, apply '+(profile?.name || 'this profile')+', then launch DCS';
}
// DCS can start or close outside the app: Launch and Restore lock while it runs.
setInterval(async () => {
  if (!state || busy || document.hidden) return;
  try { const {dcsRunning} = await request('dcsStatus'); if (state && dcsRunning !== Boolean(state.dcsRunning)) { state.dcsRunning = dcsRunning; sendChangedSoon(true); renderOriginals(); updateWorkflow(); renderDashboard(); loadFlight(dcsRunning ? 0 : 3000); } } catch { }
}, 4000);
// ---- Checks: one list, grouped Must fix / Check yourself / OK, each line with its way to the fix ---------------------
// Titles and links for check ids (ProfileValidation codes, Readiness ids). [title, {reveal|page|guide, label, browse}]
const CHECK_INFO = {
  'neural-runtime':['DLSS 5 runtime file',{reveal:'neuralRuntimePath',label:'Select file',browse:true}],
  'dcs-not-found':['DCS executable',{reveal:'dcs',label:'Select',browse:true}], 'options-not-found':['DCS settings file',{reveal:'options',label:'Select',browse:true}], 'options-location':['DCS settings file',{reveal:'options',label:'Select',browse:true}],
  'game':['DCS and Saved Games',{reveal:'dcs',label:'Open'}], 'dcs-launcher':['DCS launcher',{reveal:'keepDcsLauncher',label:'Open'}],
  'dcs-running':['DCS is running',null], 'closed':['DCS is closed',null],
  'neural-hotkey':['DLSS 5 toggle key',{reveal:'neuralToggleKey',label:'Change'}], 'neural-hotkey-dcs':['DLSS 5 toggle key',{reveal:'neuralToggleKey',label:'Change'}],
  'diagnostic-hotkey':['Headset panel key',{reveal:'diagnosticOverlayKey',label:'Change'}], 'diagnostic-hotkey-dcs':['Headset panel key',{reveal:'diagnosticOverlayKey',label:'Change'}],
  'engine-hotkey':['Engine Optimizations switch',{reveal:'engineToggleKey',label:'Change'}], 'engine-hotkey-dcs':['Engine Optimizations switch',{reveal:'engineToggleKey',label:'Change'}],
  'engine-timer':['Streaming timer refresh',{reveal:'engineTimerRefreshUs',label:'Open'}], 'engine-dev':['Developer mode',{reveal:'engineDevMode',label:'Open'}], 'engine-dev-missing':['Developer mode',{reveal:'engineDevMode',label:'Open'}],
  'nvidia-threaded-optimization':['NVIDIA Threaded optimization',null], 'nvidia-mfaa':['NVIDIA MFAA',null], 'eye-distance':['Eye distance',{reveal:'eyeDistanceMm',label:'Open'}],
  'smooth-cursor-images':['Smooth mouse cursor',{reveal:'smoothCursor',label:'Open'}], 'fps-cap':['DCS frame limit',{reveal:'fpsLimit',label:'Open'}], 'dcs-vsync':['Desktop VSync',{reveal:'disableDcsVSync',label:'Open'}],
  'hotkey-conflict':['In-flight keys',{reveal:'diagnosticOverlayKey',label:'Change'}],
  'pimax-fovea':['Focus area from Pimax Play',{page:'foveation',label:'Open'}], 'quad-coverage-cap':['Focus area size',{page:'foveation',label:'Open'}],
  'quad-cheeky':['Quad Views and DLSS',{page:'foveation',label:'Open'}], 'quad-runtime':['Quad Views provider',{reveal:'quadViews',label:'Open'}], 'quad-adapter-provider':['Quad Views provider',{reveal:'quadViewsLayerDirectory',label:'Open'}],
  'quad-focus':['Focus adapter',null], 'pimax-native-replaced':['Quad Views provider',null], 'fovea-source':['Focus area source',{reveal:'foveaSource',label:'Open'}],
  'boost-launcher':['CPU Boost app lists',{page:'boost',label:'Open'}], 'boost-app-name':['CPU Boost app lists',{page:'boost',label:'Open'}], 'boost-app-wildcard':['CPU Boost app lists',{page:'boost',label:'Open'}],
  'boost-choice':['CPU Boost',{page:'boost',label:'Open'}], 'engine-prefetch':['Terrain prefetch fix',{reveal:'boostPrefetch',label:'Open'}], 'monitor-mode':['Monitor mode in flight',{reveal:'flightDisplayMode',label:'Open'}], 'boost-high-priority':['DCS priority',{reveal:'boostDcsPriority',label:'Open'}],
  'framegen-factor':['Generated frames',{reveal:'frameGenFactor',label:'Open'}], 'flow-preset':['Optical flow',{reveal:'flowPreset',label:'Open'}], 'flow-scale':['Optical flow',{reveal:'nvidiaFlowScale',label:'Open'}],
  'fps-mode':['DCS frame limit',{reveal:'fpsLimit',label:'Open'}], 'fps-under-target':['DCS frame limit',{reveal:'fpsLimit',label:'Open'}], 'adaptive-cadence-cap':['DCS frame limit',{reveal:'fpsLimit',label:'Open'}],
  'refresh-rate':['Refresh rate',{reveal:'refreshFromHeadset',label:'Open'}], 'external-limiters':['Other FPS limits',null], 'runtime-reprojection':['Motion smoothing',null],
  'route-mismatch':['Headset route',{page:'setup',label:'Open'}], 'steamvr-not-found':['SteamVR',{guide:'steamvr',label:'Guide'}],
  'gaze-bridge-missing':['Eye tracking on Sboys',{guide:'gaze',label:'Guide'}], 'runtime':['OpenXR runtime',{page:'setup',label:'Open'}],
  'active-backup':['Applied profile',{page:'recovery',label:'Stock DCS'}], 'deployment':['Files and components',{page:'diagnostics',label:'Review',review:true}],
  'write-access':['Write access to game folders',{action:'grantWriteAccess',label:'Grant access'}], 'write-blocked':['Write access to game folders',null],
  'dlss-gpu':['DLSS 5 graphics card',{page:'dlss',label:'Open'}], 'framegen-gpu':['Frame generation graphics card',{reveal:'frameGen',label:'Open'}],
  'foreign-dxgi':['Another program\'s dxgi.dll',null],
  'tobii-paused':['Desktop Tobii eye tracker',{action:'resumeTobii',label:'Start Tobii'}],
  'pupil-gaze':['Pupil shift',{page:'pupil',label:'Open'}], 'pupil-radius':['Pupil shift',{reveal:'pupilShiftEyeRadiusMm',label:'Open'}], 'pupil-image':['Pupil shift',{reveal:'pupilShiftVirtualImageM',label:'Open'}],
  'periphery-stability-strength':['Periphery stability',{reveal:'peripheryStabilityStrength',label:'Open'}], 'periphery-stability-provider':['Periphery stability',{page:'foveation',label:'Open'}], 'far-depth-needs':['Far depth',{page:'pupil',label:'Open'}],
  'pupil-hotkey':['Pupil shift switch',{reveal:'pupilShiftToggleKey',label:'Open'}], 'pupil-hotkey-dcs':['Pupil shift switch',{reveal:'pupilShiftToggleKey',label:'Open'}],
  'depth-gaze':['Depth boost',{page:'pupil',label:'Open'}], 'depth-max':['Depth boost',{reveal:'depthBoostMax',label:'Open'}], 'depth-near':['Depth boost',{reveal:'depthBoostNearM',label:'Open'}],
  'depth-mode':['Depth boost',{reveal:'depthBoostMode',label:'Open'}], 'depth-realistic-max':['Depth boost',{reveal:'depthBoostRealisticMax',label:'Open'}], 'depth-far':['Depth boost',{reveal:'depthBoostFarStartM',label:'Open'}],
  'depth-hotkey':['Depth boost switch',{reveal:'depthBoostToggleKey',label:'Open'}], 'depth-hotkey-dcs':['Depth boost switch',{reveal:'depthBoostToggleKey',label:'Open'}],
  'color-hotkey':['Color mode key',{reveal:'colorModeKey',label:'Change'}], 'color-hotkey-dcs':['Color mode key',{reveal:'colorModeKey',label:'Change'}], 'visual-gaze':['Eye-focus blur',{page:'foveation',label:'Open'}],
  'hud-symbology-needs':['Sharp HUD',{reveal:'depthBoost',label:'Open'}],

  'focus-preset':['Focus DLSS preset',{reveal:'focusDlssPreset',label:'Open'}],
  'periphery-preset':['Periphery DLSS preset',{reveal:'peripheryDlssPreset',label:'Open'}], 'periphery-preset-needs':['Periphery DLSS preset',{reveal:'peripheryDlssPreset',label:'Open'}],
  'periphery-preset-dlss':['Periphery DLSS preset',null], 'nvidia-dlss-preset':['NVIDIA DLSS preset',{reveal:'peripheryDlssPreset',label:'Open'}],
  'neural-area':['DLSS 5 area',{reveal:'neuralFocusArea',label:'Open'}], 'neural-depth':['Depth convention',{reveal:'neuralDepth',label:'Open'}],
  'fovea-width':['Focus width',{reveal:'foveaWidth',label:'Open'}], 'fovea-height':['Focus height',{reveal:'foveaHeight',label:'Open'}], 'peripheral-scale':['Peripheral resolution',{reveal:'peripheralScale',label:'Open'}], 'quad-focus-scale':['Center resolution',{reveal:'quadFocusScale',label:'Open'}], 'quad-sharpening':['Focus sharpening',{reveal:'quadSharpening',label:'Open'}], 'quad-periphery-contrast':['Periphery contrast',{reveal:'quadPeripheryContrast',label:'Open'}], 'quad-edge-blend':['Focus edge blending',{reveal:'quadEdgeBlend',label:'Open'}], 'neural-scale':['Working scale',{reveal:'neuralWorkingScale',label:'Open'}], 'neural-intensity':['Intensity',{reveal:'neuralIntensity',label:'Open'}], 'neural-local-tone':['Local tone',{reveal:'neuralLocalTone',label:'Open'}], 'neural-local-structure':['Local structure',{reveal:'neuralLocalStructure',label:'Open'}], 'neural-skin-structure':['Skin structure',{reveal:'neuralSkinStructure',label:'Open'}], 'neural-color':['Color strength',{reveal:'neuralColorStrength',label:'Open'}], 'neural-transfer':['Transfer strength',{reveal:'neuralTransferStrength',label:'Open'}], 'neural-paper-white':['Paper white scale',{reveal:'neuralPaperWhiteScale',label:'Open'}], 'neural-motion-x':['Motion scale X',{reveal:'neuralMotionScaleX',label:'Open'}], 'neural-motion-y':['Motion scale Y',{reveal:'neuralMotionScaleY',label:'Open'}], 'rendered-fps-cap':['Custom limit',{reveal:'renderedFpsCap',label:'Open'}], 'far-depth-hotkey':['Far depth switch',{reveal:'farDepthToggleKey',label:'Change'}], 'far-depth-hotkey-dcs':['Far depth switch',{reveal:'farDepthToggleKey',label:'Change'}], 'far-depth-preset':['Far depth',{reveal:'farDepthPreset',label:'Open'}], 'schema':['Profile file',null], 'profile-id':['Profile name',null], 'profile-name':['Profile name',null], 'enum':['Profile file',null]
};
/** A key conflict opens the key its message names first (ProfileValidation's wording). */
function conflictInfo(text) {
  const key = [['Far depth','farDepthToggleKey'],['color mode key','colorModeKey'],['Depth boost switch','depthBoostToggleKey'],['Pupil shift switch','pupilShiftToggleKey'],
    ['Engine Optimizations switch','engineToggleKey'],['DLSS 5 toggle','neuralToggleKey']].find(([name]) => String(text).startsWith('The '+name))?.[1] || 'diagnosticOverlayKey';
  return ['In-flight keys',{reveal:key,label:'Change'}];
}
/** Visual realism's range checks are "visual-" + the field (ProfileValidation): they open that field. */
function visualInfo(id) {
  const f = /^visual-/.test(id) ? fields.find(x => x.path === id.slice(7)) : null;
  return f ? [plainTitle(f.title),{reveal:f.path,label:'Open'}] : undefined;
}
const GROUPS = [['fix','Must fix'],['check','Check yourself'],['ok','OK']];
/** Profile issues (every edit) and, after Check this PC, the readiness checks; one entry per id. */
function checkItems() {
  const items = [], seen = new Set();
  const add = (id,group,title,text,guide) => { const key = id+'|'+text; if (seen.has(key)) return; seen.add(key); const info = id === 'hotkey-conflict' ? conflictInfo(text) : CHECK_INFO[id] ?? visualInfo(id); items.push({id,group,title:info?.[0] || title,text,action:info?.[1] || (guide && !['paths','recovery'].includes(guide) ? {guide,label:'Guide'} : null)}); };
  // Profile checks follow the draft; readiness repeats them under the generic "Settings" title (Setup.cs).
  for (const i of state?.issues || []) add(i.code, i.severity === 'Error' ? 'fix' : i.severity === 'Warning' ? 'check' : 'ok', 'Profile', i.message);
  for (const c of state?.readiness?.checks || []) if (!(c.title === 'Settings' && (state.issues || []).some(i => i.code === c.id)))
    add(c.id, c.state === 'Error' ? 'fix' : c.state === 'Pass' ? 'ok' : 'check', c.title, c.detail, c.guide);
  return items;
}
function actionHtml(a) {
  if (!a) return '';
  if (a.reveal) return `<button class="link-button" data-reveal="${escape(a.reveal)}"${a.browse ? ' data-browse-after="1"' : ''}>${escape(a.label)} →</button>`;
  if (a.guide) return `<button class="link-button" data-guide="${escape(a.guide)}">${escape(a.label)} ↗</button>`;
  if (a.review) return `<button class="link-button" data-review="1">${escape(a.label)} →</button>`;
  if (a.action) return `<button class="link-button" data-run="${escape(a.action)}">${escape(a.label)} →</button>`;
  return a.page === 'diagnostics' ? '' : `<button class="link-button" data-open-page="${escape(a.page)}">${escape(a.label)} →</button>`;
}
function renderChecks() {
  const items = checkItems(), mark = {fix:'!',check:'?',ok:'✓'};
  const row = c => `<div class="check-row ${c.group}" title="${escape(c.text)}"><span class="check-mark" aria-hidden="true">${mark[c.group]}</span><button type="button" class="check-line" aria-expanded="false"><b>${escape(c.title)}</b><span>${escape(c.text)}</span></button>${actionHtml(c.action)}</div>`;
  const count = g => items.filter(c => c.group === g).length, open = $('checkGroups').querySelector('details.check-ok')?.open;
  $('checkGroups').innerHTML = GROUPS.map(([g,label]) => {
    const list = items.filter(c => c.group === g);
    if (g === 'ok') return list.length ? `<details class="check-group check-ok"${open ? ' open' : ''}><summary><span class="check-count">${list.length}</span>OK</summary>${list.map(row).join('')}</details>` : '';
    return `<section class="check-group check-${g}" aria-label="${label}"><h3><span class="check-count">${list.length}</span>${label}</h3>${list.length ? list.map(row).join('') : `<p class="fine check-none">${g === 'fix' ? 'Nothing blocks Launch DCS.' : 'Nothing to check.'}</p>`}</section>`;
  }).join('');
  const r = state?.readiness;
  $('checksNote').textContent = r ? `This PC was checked with these settings: ${count('fix')} to fix, ${count('check')} to check yourself.` : 'Settings are checked as you change them. Check this PC also checks the headset software, drivers and write access.';
}
document.addEventListener('click', e => {
  const line = e.target.closest('.check-line'); if (line) { const r = line.closest('.check-row'); r.classList.toggle('expanded'); line.setAttribute('aria-expanded',String(r.classList.contains('expanded'))); }
  const guide = e.target.closest('[data-guide]'); if (guide && !busy && guide.closest('#checkGroups')) request('openGuide',{guide:guide.dataset.guide}).catch(showError);
  if (e.target.closest('#checkGroups [data-review]') && !busy) run('preview').catch(()=>{});
  const action = e.target.closest('#checkGroups [data-run]'); if (action && !busy) run(action.dataset.run).catch(()=>{});
});
/** The route and the features a profile runs, e.g. "Pimax · Quad Views + DLSS 5". With Optimizations only the VR
 *  features stay in the profile for the next VR flight but do not run, so they are not named. */
function runsWith(p) {
  if (!p) return 'DCS as it is';
  const on = featureState(p), vr = !p.desktop, route = p.desktop ? 'Optimizations only' : p.runtime === 'SboysSteamVr' ? 'Sboys' : 'Pimax';
  const running = [vr && on.quad && (p.quadViews === 'PimaxNative' ? 'Pimax Quad Views' : 'Quad Views'), vr && (p.neuralRendering ? 'DLSS 5' : foveatedSr(p) && 'Foveated Super Resolution'), vr && on.framegen && 'Frame generation', on.boost && 'CPU Boost', on.engine && 'Engine Optimizations'].filter(Boolean);
  return route+' · '+(running.length ? running.join(' + ') : 'DCS as it is');
}
/** Overview: what Launch DCS will do now, the profile, and the checks in one line. */
function renderDashboard() {
  if (!profile || !state) return;
  const mode = launchState(), items = checkItems(), fix = items.filter(c => c.group === 'fix').length, check = items.filter(c => c.group === 'check').length;
  // The title: the short state, then what runs, e.g. "Ready to fly · Next launch · Pimax · Quad Views + Frame generation".
  // While DCS runs it is what was applied ("In DCS now · …"), not the draft.
  const title = mode === 'running' ? 'In DCS now · '+runsWith(state.appliedDraft) : STATE_WORDS[mode]+' · Next launch · '+runsWith(profile);
  // Guided setup: the first step while nothing has ever been applied, a quiet link afterwards.
  const first = !state.appliedProfile && !state.originals?.count;
  // Under it the profile's own name only when the title does not already say it, and what is applied.
  const small = [profile.name !== deriveIdentity(profile).name ? 'Profile: '+profile.name : '', state.appliedProfile ? 'Applied: '+(state.appliedName || state.appliedProfile) : ''].filter(Boolean).join(' · ');
  const guided = `<button id="guidedSetup" class="${first ? 'button primary dash-setup' : 'link-button'}">${first ? 'First time? Guided setup' : 'Guided setup'}</button>`;
  const details = {running:'Close DCS to change the configuration. The flight summary appears here when it exits.',
    ready:state.foveaChanged ? state.foveaChanged : 'These settings are applied. Launch DCS starts DCS with them.', changes:state.appliedDraft ? 'You changed settings since the last launch. Launch DCS applies them.' : 'Nothing is applied yet. Launch DCS backs up DCS\'s original files, then applies these settings.',
    invalid:'Correct the marked values before launching.'};
  const chips = [fix ? `<button class="dash-chip fix" data-open-page="diagnostics">${fix} must fix</button>` : '', check ? `<button class="dash-chip check" data-open-page="diagnostics">${check} to check yourself</button>` : '', !fix && !check ? '<button class="dash-chip ok" data-open-page="diagnostics">Nothing to fix</button>' : ''].join('');
  const reset = state.appliedDraft && mode === 'changes' ? '<button class="link-button" data-reset-applied="1">Undo changes</button>' : '';
  $('dashStatus').className = 'dash-status '+mode;
  $('dashStatus').innerHTML = `<span class="dash-dot" aria-hidden="true"></span><div class="dash-main"><b>${escape(title)}</b><p>${escape(details[mode])}</p>${small ? `<small>${escape(small)}</small>` : ''}</div><div class="dash-side">${chips}${reset}${guided}</div>`;
}
document.addEventListener('click', e => { if (e.target.closest('[data-reset-applied]')) resetToApplied(); });
// ---- Last flight: read from the logs once DCS has exited --------------------------------------------------------------
let flightTimer, flightSequence = 0;
function loadFlight(delay = 0) {
  clearTimeout(flightTimer);
  flightTimer = setTimeout(async () => {
    const mine = ++flightSequence;
    try { const result = await request('lastFlight'); if (mine === flightSequence) { flight = result; renderFlight(); } } catch { }
  }, delay);
}
/** A field title without its markup (the EXPERIMENTAL tag), for search. */
const plainTitle = title => String(title).replace(/<[^>]+>/g,'').trim();
const fmt = (v,d = 0) => Number(v).toLocaleString('en-US',{maximumFractionDigits:d,minimumFractionDigits:d});
const share = v => Math.round(v*100)+'%';
function duration(seconds) { const t = Math.round(seconds), m = Math.floor(t/60), h = Math.floor(m/60); return h ? `${h} h ${m%60} min` : m ? `${m} min ${t%60} s` : `${t} s`; }
function renderFlight() {
  const body = $('flightBody'), f = flight?.flight;
  if (flight?.running) { $('flightTitle').textContent = 'DCS is running'; body.innerHTML = '<p class="fine">The summary appears here after DCS exits.</p>'; return; }
  if (!f) { $('flightTitle').textContent = 'No flight recorded yet'; body.innerHTML = '<p class="fine">After a flight, this shows the frame rate, frame time and what each feature did, read from the logs.</p>'; return; }
  const start = new Date(f.start), seconds = (new Date(f.end) - start) / 1000;
  $('flightTitle').textContent = start.toLocaleString('en-GB',{weekday:'short',day:'numeric',month:'short',hour:'2-digit',minute:'2-digit'}) + ' · ' + duration(seconds) + (f.closed ? '' : ' · DCS did not close normally');
  const rows = [];
  if (f.headset) rows.push(['Headset',`${fmt(f.headset.averageFps,1)} FPS`,`${share(f.headset.atRefreshShare)} of the time at ${fmt(f.headset.compositorFps)} Hz · ${fmt(f.headset.missed)} missed, ${fmt(f.headset.discarded)} discarded`]);
  if (f.framegen) {
    // Seconds at each level; 1× is full rate with generation paused (adaptive cadence).
    const g = f.framegen, total = (g.seconds1x || 0) + g.seconds2x + g.seconds3x;
    // With frame generation only when needed, the 1× share is the time DCS kept up on its own: no generated frames ("None").
    const parts = [['None',g.seconds1x || 0],['2×',g.seconds2x],['3×',g.seconds3x]].filter(([,v]) => v > 0);
    // Cadence on without any 1× time (DCS never kept up) is still "Only when needed", not Auto.
    const one = g.seconds1x || 0, mode = g.cadence || one ? 'Only when needed' : g.adaptive ? 'Auto' : 'Fixed';
    rows.push(['Frame generation',parts.length > 1 ? parts.map(([k,v]) => `${k} ${share(v/total)}`).join(' · ') : parts.length ? `${parts[0][0]} all flight` : '—',mode+(g.switches ? ` · level changed ${g.switches} ${g.switches === 1 ? 'time' : 'times'}` : '')]);
  }
  if (f.frameTime) rows.push(['DCS frame time',`${fmt(f.frameTime.p50Ms,1)} ms`,`median · ${fmt(f.frameTime.p90Ms,1)} ms p90 · the slower of CPU and GPU`]);
  if (f.dlss5) rows.push(['DLSS 5',f.dlss5.state === 'ran' ? 'Ran' : f.dlss5.state === 'failed' ? 'Failed' : 'Not started',f.dlss5.state === 'failed' ? f.dlss5.error : [f.dlss5.version ? 'runtime '+f.dlss5.version : 'no 3D scene reached', f.dlss5.toggles ? `toggled ${f.dlss5.toggles}×, ${f.dlss5.onAtExit ? 'on' : 'off'} at exit` : null].filter(Boolean).join(' · '),f.dlss5.state === 'failed']);
  if (f.boost) rows.push(['CPU Boost',f.boost.state === 'restored' ? 'Restored' : f.boost.state === 'running' ? 'Running' : f.boost.state,`${f.boost.moved} apps moved, ${f.boost.restored} restored`+(f.boost.errors.length ? ` · ${f.boost.errors.length} need admin rights` : ''),f.boost.errors.length > 0]);
  if (f.prefetch) rows.push(['Terrain prefetch fix',f.prefetch.averageCallsPerSecond ? `${fmt(f.prefetch.skippedPercent,1)}% skipped` : 'No calls',f.prefetch.averageCallsPerSecond ? `${fmt(f.prefetch.averageCallsPerSecond/1000,0)}k calls/s average · ${fmt(f.prefetch.peakCallsPerSecond/1000,0)}k peak` : 'Loaded; DCS made no repeated prefetch calls']);
  // A frame rate held by the headset software (motion smoothing at half the refresh rate, for example): CPU and GPU
  // have headroom, so no setting here can raise it. Not with frame generation (DCS runs at a fraction of the refresh
  // rate on purpose) and not at the full refresh rate (divider 1).
  const capFps = v => fmt(v, Number.isInteger(v) ? 0 : 1);
  if (f.cap && f.cap.reports && f.cap.cappedReports / f.cap.reports >= 0.5 && !f.framegen && f.cap.divider !== 1)
    rows.push(['Frame rate held',f.cap.fps ? `${capFps(f.cap.fps)} FPS` : 'Capped',`${share(f.cap.cappedReports/f.cap.reports)} of the flight`+(f.cap.refreshHz && f.cap.divider ? ` at ${fmt(f.cap.refreshHz)} Hz / ${f.cap.divider}` : '')+' by the headset software (motion smoothing). CPU and GPU had room to spare, so optimizations cannot raise it. Turn off motion smoothing or raise the refresh rate to go higher.',true]);
  // Eye tracking that did not reach the bundled Quad Views: once the desktop Tobii software has taken the headset's
  // tracker, only a headset restart gives it back.
  if (f.eyeTracking && eyeTrackedVr(profile) && profile.quadViews === 'QuadViewsFoveated')
    rows.push(['Eye tracking',f.eyeTracking.supported ? 'Reached Quad Views' : 'Did not reach Quad Views',f.eyeTracking.supported ? 'The focus area followed your eyes.'
      : 'The focus area stayed in the center. Restart the headset in Pimax Play (desktop Tobii software can hold its eye tracker), then start DCS again.',!f.eyeTracking.supported]);
  body.innerHTML = rows.length ? `<dl class="flight-grid">${rows.map(([k,v,d,warn]) => `<div class="flight-item${warn ? ' warn' : ''}"><dt>${k}</dt><dd><b>${escape(v)}</b><small title="${escape(d)}">${escape(d)}</small></dd></div>`).join('')}</dl>` : '<p class="fine">DCS ran, but no headset, frame generation or CPU Boost log covers this session.</p>';
}
function renderDiagnostics() { renderInventory(); renderEdits(); }
/** What an edit changes: the checks, the files Launch will write and the overview. */
function renderEdits() {
  renderChecks();
  $('fileCount').textContent = plan ? plan.files.length+' FILES' : 'NOT REVIEWED';
  $('plannedFiles').innerHTML = plannedFilesHtml();
  $('report').textContent = state.report;
  renderDashboard();
}
/** What was detected on this PC (redrawn after a rescan, not on every edit). */
function renderInventory() {
  const i = state.inventory;
  const info = [['DCS executable',i.dcsExecutable || 'Not detected'],['DCS settings file',i.optionsPath || 'Not detected'],['Pimax runtime',i.pimaxRuntime || 'Not detected'],['SteamVR runtime',i.steamVrRuntime || 'Not detected'],['Active OpenXR runtime',i.activeRuntime || 'Not detected'],['Pimax version',i.pimaxVersion || 'Not detected'],['Pimax Play Quad Views',state.pimax?.found ? state.pimax.summary : 'Settings not found'],['DCS',i.dcsRunning ? 'Running' : 'Closed'],['DCS frame limit',i.dcsSettings['graphics.maxFPS'] || 'Not detected'],['DCS desktop VSync',i.dcsSettings['graphics.sync'] || 'Not detected'],['autoexec.cfg frame limit',i.autoexecMaxFps || (i.autoexecPath ? 'Unknown · check the file' : 'No autoexec.cfg')],['RTSS',(i.limiterProcesses || []).join(', ') || 'Not running · check NVIDIA\'s frame limits yourself']];
  $('inventory').innerHTML = info.map(([k,v]) => `<dt>${k}</dt><dd>${escape(v)}</dd>`).join('');
  $('inventoryObservations').innerHTML = i.observations.map(note => `<p>${escape(note)}</p>`).join('');
  $('inventoryFiles').innerHTML = i.files.map(file => `<div class="file"><b>${escape(file.path)}</b><p>${file.exists ? 'Present' : 'Missing'} · ${escape(file.version || 'No version')}<br><span>${escape(file.sha256 || 'No hash captured')}</span></p></div>`).join('') + i.layers.map(layer => `<div class="file"><b>${escape(layer.name || layer.manifestPath)}</b><p>${escape(layer.scope)} · ${layer.enabled ? 'Enabled' : 'Disabled'} · library ${layer.libraryExists ? 'present' : 'missing'}</p><span>${escape(layer.libraryPath)}</span></div>`).join('');
  if (!i.files.length && !i.layers.length) $('inventoryFiles').innerHTML = '<p class="fine">No files or OpenXR layers were found.</p>';
  $('inventoryTime').textContent = new Date(i.capturedAt).toLocaleTimeString('en-GB');
}
function plannedFilesHtml() {
  return plan ? plan.files.map(f => `<div class="file"><b>${f.replaces ? '~' : '+'} ${escape(f.path)}</b><p>${escape(f.purpose)} <span>· ${f.bytes.toLocaleString('en-US')} bytes</span></p>${(f.luaChanges || []).map(c => `<p>${escape(c.path)}: ${escape(c.previousRaw ?? 'absent')} → ${escape(c.installedRaw)}</p>`).join('')}</div>`).join('') : '<p class="fine">Nothing is written until you launch. Review files lists every file and DCS setting Launch DCS will write.</p>';
}
/** Recovery: one record of the original files DCS Control changed, restored with one button. */
const ORIGINAL_ACTIONS = {restore:'put the original back', remove:'remove it (DCS Control created it)', settings:'set back only the settings DCS Control changed'};
function renderOriginals() {
  const o = state.originals || {count:0, files:[]}, running = Boolean(state.dcsRunning), open = $('originalFiles')?.open;
  const when = o.lastActionAt ? new Date(o.lastActionAt).toLocaleString('en-GB') : null;
  const last = o.lastAction ? `<p class="fine">Last action: ${escape(o.lastAction)}${when ? ' · '+escape(when) : ''}</p>` : '';
  const folder = `<button class="link-button" id="openBackups" title="${escape(o.folder || '')}">Backups folder ↗</button>`;
  const rows = o.files.map(f => `<div class="file original-file"><b>${escape(f.path)}</b><p>${escape(ORIGINAL_ACTIONS[f.action] || f.action)}${f.detail ? ' · '+escape(f.detail) : ''}</p></div>`).join('');
  $('originals').innerHTML = o.count
    ? `<div class="card originals-card" id="originalsCard"><div class="originals-head"><div><span class="tag">${state.appliedProfile ? 'APPLIED: '+escape((state.appliedName || state.appliedProfile).toUpperCase()) : 'CHANGED'}</span><h3>Original files · ${o.count} ${o.count === 1 ? 'file' : 'files'} changed by DCS Control</h3><p>Back to stock DCS puts each one back as it was before DCS Control first changed it. In DCS's settings file (options.lua) only the settings DCS Control changed go back; your own changes stay. Engine Optimizations and the other features stay off in DCS until you launch from here again.</p>${last}</div><button class="button secondary" id="restoreOriginals" ${running || busy ? 'disabled' : ''} ${running ? 'title="Close DCS before restoring"' : ''}>Back to stock DCS</button></div><details id="originalFiles"${open ? ' open' : ''}><summary>What Back to stock DCS does, file by file</summary><div class="file-list">${rows}</div></details><div class="inline-actions">${folder}</div></div>`
    : `<div class="card originals-card empty" id="originalsCard"><div class="empty-state"><h2>No changes to put back</h2><p>DCS Control has not changed any file. Launch DCS (or Apply without launching) backs up each original the first time it changes it.</p>${last}</div><div class="inline-actions">${folder}</div></div>`;
  $('restoreOriginals')?.addEventListener('click',() => run('restore').catch(()=>{}));
  $('openBackups').addEventListener('click',() => request('openBackups').catch(showError));
}
/** A setting or block hidden on its page (or in the right panel): its own element, its card or a fold around it. Search
 *  leaves it out. Focus values that follow Pimax Play stay findable: revealSetting shows the Pimax block for them. */
function hiddenOnPage(f) {
  if (!f.element) return true;
  if (f.element.hidden && f.page === 'foveation' && pimaxSource(profile)) return false;
  for (let e = f.element; e && !e.classList?.contains('page'); e = e.parentElement) if (e.hidden) return true;
  return false;
}
function openSearch() { $('searchDialog').showModal(); $('searchInput').value = ''; renderSearch(); $('searchInput').focus(); }
/** Earlier page names, so a search for them still finds the page's settings. */
const PAGE_ALIASES = {support:'troubleshooting',osd:'osd hotkeys',dlss:'dlss 5',setup:'detect my setup'};
function renderSearch() {
  const normalize = text => text.toLowerCase().replace(/[^a-z0-9]/g,'');
  const query = normalize($('searchInput').value);
  const shortcuts = [
    ['overview','Features','Turn Engine Optimizations, CPU Boost, Quad Views, DLSS 5 and frame generation on or off in the right panel.','featureList'],
    ['dlss','Advanced image controls','DLSS 5 model corrections: tone, structure, masks, color, depth and motion guides.','neuralAdvanced'],
    ['foveation','Pimax Play focus values','Pimax Play\'s Quad View settings as Pimax Play shows them, and how bundled Quad Views reproduces them. Re-read them.','pimaxFovea'],
    ['boost','What happens at launch','Cores, processes, VRAM to free, the monitor mode and the DCS window the CPU Boost page would change on this PC.','boostPlan'],
    ['osd','In-flight keys','Every in-flight key (hotkey) and what it does.','keysControls'],
    ['osd','Headset panel (OSD)','The status panel in the headset: FPS and what is on. Show or hide it in flight with its key.','panelControls'],
    ['diagnostics','Files Launch DCS writes','Review files: every file and DCS setting Launch DCS writes for these settings.','planCard'],
    ['diagnostics','Checks','Must fix, check yourself and OK: what stands between these settings and a flight.','checkGroups'],
    ['overview','Last flight','Headset FPS, how often frames were generated, DCS frame time, DLSS 5, CPU Boost and the terrain prefetch fix (Engine Optimizations) from the last flight.','flightCard'],
    ['support','Diagnostic report','Save a text report: settings, checks and what was detected on this PC, as a file.','export'],
    ['support','Diagnostic package','One ZIP with the logs and settings needed to understand a problem, for a report.','diagnosticPackage'],
    ['support','DLSS 5 not applied','Why DLSS 5 did not reach the focus area in the last session.','dlssFocus'],
    ['setup','Read settings from this PC','Detect DCS, Pimax Play and headset values on this PC and fill them in. Nothing is applied until Launch.','detectSetup'],
    ['setup','Guided setup','Three steps: choose what to run, check it, then fly.','guidedSetupHere'],
    ['recovery','Stock DCS','Back to stock DCS: put back every file and setting DCS Control changed.','originals'],
    ['guide','Feature guide','What each feature does, what to expect from it and what it costs.','featureGuide']
  ].map(([page,title,description,id]) => ({page,title,description,path:id,element:$(id),input:$(id)}));
  searchResults = [...fields,...shortcuts].filter(f => !(profile?.desktop && VR_PAGES.includes(f.page)) && !hiddenOnPage(f)).filter(f => !query || normalize(plainTitle(f.title)+' '+f.description+' '+(f.detail || '')+' '+f.path+' '+pages.find(p => p[0] === f.page).slice(1,4).join(' ')+' '+(PAGE_ALIASES[f.page] || '')).includes(query));
  $('searchResults').innerHTML = searchResults.length ? searchResults.map((f,index) => `<button class="search-result" data-index="${index}"><div><b>${escape(plainTitle(f.title))}</b><small>${escape(f.description)}</small></div><span>${f.path === 'featureList' ? 'Right panel' : pages.find(p => p[0] === f.page)[1]} ›</span></button>`).join('') : '<div class="empty-state"><p>No settings match this search.</p></div>';
  $('searchResults').querySelectorAll('button').forEach(b => b.addEventListener('click',() => revealSetting(searchResults[Number(b.dataset.index)])));
}
// ---- Feature guide --------------------------------------------------------------------------------------------------
/** What each feature gives, what to expect from it and what it costs, in a few lines each (Feature guide page).
 *  impact: the one-word benefit on the card's badge; keyPath/keyText: its in-flight key (the profile's), if any. */
const GUIDE_SECTIONS = [
  ['every','Every pilot','On a monitor or any headset.'],
  ['vr','VR','Pimax headsets (and Sboys for SteamVR).'],
  ['perception','Perception','Experimental: new, off by default, and still being tuned. Feedback welcome.'],
  ['visual','Visual realism','Off by default. The color mode key switches color modes in flight.'],
  ['safety','Safety net','']
];
const FEATURE_GUIDE = [
  {section:'every', page:'engine', name:'Engine Optimizations', impact:'More FPS', on:p => p.engineOptimizations,
   gives:'DCS needs less CPU time for every frame. The image stays exactly the same.',
   expect:'The busier the scene, the bigger the gain: airfields, carriers and large missions run noticeably smoother. Little or no change when the graphics card is the limit.',
   cost:'Nothing visible. The first start prepares some data for a few minutes.',
   needs:'NVIDIA Threaded optimization not Off and MFAA off for DCS. Checks tells you.',
   keyPath:'engineToggleKey', keyText:'off and on, to compare'},
  {section:'every', page:'boost', name:'CPU Boost', impact:'Steadier', on:p => p.cpuBoost,
   gives:'DCS gets priority and its best CPU cores; other programs step out of its way while you fly.',
   expect:'Steadier frame times when other programs compete for the CPU. On a quiet PC the difference is small.',
   cost:'None. Undone when DCS exits.'},
  {section:'vr', page:'foveation', name:'Quad Views', impact:'More FPS', on:p => p.quadViews !== 'None',
   gives:'Full detail where you look, lower resolution around it.',
   expect:'Usually the biggest gain in VR. With eye tracking the sharp area follows your eyes and you rarely notice the edge.',
   cost:'A softer periphery, where thin lines can shimmer.',
   needs:'Eye tracking (Pimax Crystal) for a focus area that follows your eyes.'},
  {section:'vr', page:'framegen', name:'Frame generation', impact:'Smoother', on:p => p.frameGen !== 'Off',
   gives:'Generated frames between the ones DCS renders, so the headset gets a frame at every refresh.',
   expect:'Smooth motion even when DCS can\'t keep up. With Auto and Only when needed (the default), DCS renders at the refresh rate and frames are generated only while it falls behind, so you get less latency whenever DCS keeps up.',
   cost:'A little latency while frames are generated (more in 3×); the mouse moves at DCS\'s own frame rate unless Smooth mouse cursor is on.',
   needs:'An NVIDIA RTX card, or FidelityFX on any card. Only when needed needs the DCS frame limit at Match refresh. With Engine Optimizations on, generated frames also use DCS\'s cockpit outline, depth and motion.'},
  {section:'vr', page:'framegen', name:'Sharp HUD', impact:'Sharper HUD', on:p => p.frameGen !== 'Off' && p.frameGenHudSymbology && usesDepthBoost(p),
   gives:'HUD and helmet symbols stay sharp in generated frames instead of smearing.',
   expect:'A clean HUD while you turn. The symbols update at DCS\'s own HUD rate, about 20 times a second.',
   cost:'About 0.15 ms of GPU and 160 MB of video memory.',
   needs:'Frame generation and Depth boost.'},
  {section:'vr', page:'dlss', name:'DLSS 5', impact:'New look', on:p => p.neuralRendering,
   gives:'NVIDIA\'s neural rendering reworks the image; with Quad Views, only where you look.',
   expect:'A different, more natural look of light and materials. Pair it with frame generation to keep the motion smooth.',
   cost:'GPU time and video memory.',
   needs:'An NVIDIA RTX card, your own nvngx_dlssnr.dll and DCS\'s Upscaling set to DLSS. Checks shows how to keep the HUD sharp.',
   keyPath:'neuralToggleKey', keyText:'off and on'},
  {section:'vr', page:'dlss', name:'Periphery DLSS preset', impact:'More FPS', on:p => usesPeripheryDlssPreset(p),
   gives:'A lighter DLSS model outside the focus area; the focus area stays as sharp as before.',
   expect:'About 0.3 ms faster per frame. The edge of view can shimmer a little more on head turns.',
   cost:'A little more shimmer at the edge of view.',
   needs:'Quad Views, Engine Optimizations and DCS\'s Upscaling set to DLSS.'},
  {section:'vr', page:'osd', name:'Headset panel', impact:'Status', vr:true, on:p => !p.desktop && (p.frameGen !== 'Off' || p.headsetPanel),
   gives:'A small panel in the headset: FPS and what each feature is doing right now.',
   expect:'A quick look to see that everything runs, and to compare features with their keys.',
   cost:'None.',
   keyPath:'diagnosticOverlayKey', keyText:'show or hide'},
  {section:'perception', page:'pupil', name:'Depth boost', impact:'More depth', on:p => usesDepthBoost(p),
   gives:'The world outside gets more stereo depth, following where you look. The cockpit stays at your real eye distance.',
   expect:'Wingmen, tankers and targets look more 3D. Keep it off for landings for now.',
   cost:'Little GPU time.',
   needs:'Quad Views with eye tracking.',
   keyPath:'depthBoostToggleKey', keyText:'off and on'},
  {section:'perception', page:'pupil', name:'Far depth', impact:'More depth', on:p => !p.desktop && p.farDepth && quad(p) && p.engineOptimizations,
   gives:'More depth for distant aircraft, ships and terrain, from about 50 m to 2 km.',
   expect:'Far objects stand out more. The cockpit and the HUD stay exactly as they are.',
   cost:'Under 0.1 ms per frame.',
   needs:'The bundled Quad Views and Engine Optimizations.',
   keyPath:'farDepthToggleKey', keyText:'off and on'},
  {section:'perception', page:'pupil', name:'Pupil shift', impact:'Steadier cockpit', on:p => usesPupilShift(p),
   gives:'Each eye is rendered from where its pupil really is.',
   expect:'The cockpit stays still when only your eyes move. Subtle, most visible on close panels.',
   cost:'No GPU cost.',
   needs:'Quad Views with eye tracking.'},
  {section:'perception', page:'foveation', name:'Periphery stability', impact:'Calmer image', on:p => !p.desktop && p.peripheryStability && quad(p),
   gives:'Calms the shimmer of thin lines (canopy frame, power lines, the horizon) outside the sharp area.',
   expect:'A quieter periphery; the sharp area is not touched.',
   cost:'Almost no GPU cost.',
   needs:'The bundled Quad Views.'},
  {section:'visual', page:'visual', name:'Color modes', impact:'Truer color', on:p => !p.desktop && p.colorMode !== 'Off',
   gives:'Fix corrects DCS\'s VR color faults. Enhanced adds a filmic look, richer color and eyes that adapt to light and dark.',
   expect:'Fix: deeper blacks, otherwise DCS\'s own look. Enhanced: warmer sunsets and more natural light and shadow.',
   cost:'0.05 ms (Fix), about 0.1 ms (Enhanced) plus night vision and glare.',
   keyPath:'colorModeKey', keyText:'Off, Fix, Enhanced'},
  {section:'visual', page:'visual', name:'Panel color correction', impact:'Truer color', on:p => !p.desktop && p.colorMode !== 'Off' && Number(p.panelGamut) > 0,
   gives:'For the Crystal Super: corrects the over-saturated colors its wide-gamut panels give DCS\'s image.',
   expect:'Calmer, more natural colors. HUD and MFDs are not affected.',
   cost:'Almost no GPU cost: it runs in the color mode\'s own pass.',
   needs:'The Fix or Enhanced color mode. Don\'t combine with Pimax\'s own color correction.'},
  {section:'visual', page:'visual', name:'Night vision and glare', impact:'Realism', on:p => !p.desktop && p.colorMode === 'Enhanced' && Boolean(p.nightVision || p.glare),
   gives:'At night, dim areas turn blue-grey like moonlight vision. Very bright lights get a soft halo.',
   expect:'Lights, lit instruments and the HUD keep their color. By day only the glare shows, around the sun and strong lights.',
   cost:'About 0.08 ms at night (night vision) and 0.1 ms (glare).',
   needs:'The Enhanced color mode.'},
  {section:'visual', page:'visual', name:'Eye-focus blur', impact:'Depth cue', on:p => usesGazeDof(p),
   gives:'Things nearer or farther than where you look go slightly soft, like a real eye.',
   expect:'The panel softens while you look outside, the terrain while you read a display. Sharp again the moment your eyes move.',
   cost:'About 0.18 ms.',
   needs:'Quad Views with eye tracking.'},
  {section:'visual', page:'visual', name:'G vision', impact:'Realism', on:p => gVisionOn(p),
   gives:'Under high G, color fades and vision closes in toward where you look, then goes black. Red-out under negative G.',
   expect:'Works together with DCS\'s own G effect without doubling it.',
   cost:'About 0.04 ms, only while G is high.'},
  {section:'safety', page:'recovery', name:'Back to stock DCS', impact:'Undo all', on:() => false,
   gives:'Puts back every file and setting DCS Control changed, in one step.',
   expect:'DCS exactly as it was before. Your profiles stay, so Launch DCS applies them again.',
   needs:'Use it before joining a multiplayer server, updating or repairing DCS.'}
];
const GUIDE_START_DESKTOP = [
  ['1','Engine Optimizations','Turn it on first: more FPS where the CPU is the limit, same image.'],
  ['2','CPU Boost','Steadier frame times when other programs compete.']
];
const GUIDE_START = [
  ['1','Every pilot','Turn on Engine Optimizations: more FPS where the CPU is the limit, same image.'],
  ['2','In VR','Add Quad Views and Frame generation: the two that make the biggest difference.'],
  ['3','Then explore','DLSS 5 for a new look, Perception for depth. Try one at a time with its key.']
];
function renderGuide() {
  const box = $('featureGuide'); if (!box) return;
  const desktop = Boolean(profile?.desktop);
  const icon = id => pages.find(p => p[0] === id)?.[4] ?? '';
  const shown = FEATURE_GUIDE.filter(g => !(desktop && (g.vr || VR_PAGES.includes(g.page))));
  const card = g => {
    const on = Boolean(profile && g.on(profile)), index = shown.indexOf(g);
    const meta = [['Cost',g.cost],['Needs',g.needs]].filter(m => m[1]).map(([k,v]) => `<div><span>${k}</span><p>${escape(v)}</p></div>`).join('');
    const parsed = g.keyPath && profile ? (parseKey(profile[g.keyPath]) ?? parseKey(HOTKEYS[g.keyPath].fallback)) : null;
    const key = parsed && parsed[0] ? `<div class="guide-key"><kbd>${escape(keyLabel(parsed))}</kbd><span>${escape(g.keyText)}</span></div>` : '';
    return `<article class="guide-card${on ? ' on' : ''}">`
      + `<header><span class="guide-icon"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="${icon(g.page)}"/></svg></span>`
      + `<div class="guide-title"><h3>${escape(g.name)}</h3>${on ? '<span class="guide-state">ON</span>' : ''}</div><span class="guide-impact">${escape(g.impact)}</span></header>`
      + `<p class="guide-lead">${escape(g.gives)}</p><p class="guide-expect">${escape(g.expect)}</p>`
      + `<div class="guide-meta">${meta}</div>`
      + `<footer>${key}<button class="link-button" data-guide-page="${index}">Open →</button></footer></article>`;
  };
  const start = `<section class="guide-start${desktop ? ' two' : ''}" aria-label="Where to start">${(desktop ? GUIDE_START_DESKTOP : GUIDE_START).map(([n,t,d]) => `<div><b>${n}</b><div><h3>${t}</h3><p>${d}</p></div></div>`).join('')}</section>`;
  box.innerHTML = start + GUIDE_SECTIONS.map(([id,title,note]) => {
    const items = shown.filter(g => g.section === id); if (!items.length) return '';
    return `<section class="guide-section"><div class="guide-section-head"><h2>${title}</h2>${note ? `<p>${note}</p>` : ''}</div><div class="guide-grid">${items.map(card).join('')}</div></section>`;
  }).join('');
  box.querySelectorAll('[data-guide-page]').forEach(b => b.addEventListener('click',() => navigate(shown[Number(b.dataset.guidePage)].page)));
}
// ---- First-start tour -----------------------------------------------------------------------------------------------
const TOUR = [
  {title:'Welcome to DCS Control', text:'It sets DCS up for your headset (or the monitor) and makes it run faster. This short tour shows where everything is. You can open it again from Help & support.'},
  {target:'#nav-guide', title:'Feature guide', text:'What each feature does, what to expect and what it costs, in a few lines each. A good place to start.'},
  {target:'#routeSwitch', title:'How you fly', text:'Pick Pimax, Sboys (SteamVR), or Optimizations only for a monitor or another headset.'},
  {target:'#featureList', title:'What to turn on', text:'Tick the features you want. The arrow next to each one opens its settings.'},
  {target:'#nav-diagnostics', title:'Checks', text:'Before you fly, Checks lists anything to fix, with a button that takes you to the setting.'},
  {target:'#launch', title:'Launch DCS', text:'Always start DCS from here. It writes your profile into DCS first, then starts it.'},
  {target:'#nav-osd', title:'In flight', text:'Every in-flight key and what it does, with the headset panel. In the app: Ctrl+1 to Ctrl+0 open the pages, Ctrl+Shift+C opens Checks, Ctrl+K finds a setting.'},
  {target:'#nav-recovery', title:'Back to stock DCS', text:'One step puts DCS back exactly as it was before DCS Control.'}
];
let tourStep = 0;
function tourSeen(set) { try { if (set) localStorage.setItem('dcsControlTourSeen', '1'); return localStorage.getItem('dcsControlTourSeen') === '1'; } catch { return true; } }
function renderTour() {
  const step = TOUR[tourStep];
  document.querySelectorAll('.tour-target').forEach(e => e.classList.remove('tour-target'));
  const target = step.target ? document.querySelector(step.target) : null;
  if (target && !target.hidden) { target.classList.add('tour-target'); target.scrollIntoView({block:'nearest'}); }
  $('tourCount').textContent = `${tourStep + 1} of ${TOUR.length}`;
  $('tourTitle').textContent = step.title; $('tourText').textContent = step.text;
  $('tourBack').hidden = tourStep === 0; $('tourNext').textContent = tourStep === TOUR.length - 1 ? 'Done' : 'Next';
}
// Seen once it opens: closing the app mid-tour does not start it again (Take the tour again does).
function openTour() { tourStep = 0; tourSeen(true); renderTour(); if (!$('tourDialog').open) $('tourDialog').show(); $('tourNext').focus(); }
function closeTour() { document.querySelectorAll('.tour-target').forEach(e => e.classList.remove('tour-target')); tourSeen(true); if ($('tourDialog').open) $('tourDialog').close(); }
function initializeTour() {
  $('tourNext').addEventListener('click', () => { if (tourStep >= TOUR.length - 1) closeTour(); else { tourStep++; renderTour(); } });
  $('tourBack').addEventListener('click', () => { if (tourStep > 0) { tourStep--; renderTour(); } });
  $('tourSkip').addEventListener('click', closeTour);
  // Non-modal, so Esc is not handled by the dialog itself; a modal dialog above it takes Esc first.
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && $('tourDialog').open && !document.querySelector('dialog[open]:modal')) closeTour(); });
  $('tourOpen').addEventListener('click', openTour);
}
function revealSetting(f) {
  if (!f) return; if ($('searchDialog').open) $('searchDialog').close(); if (f.path !== 'featureList') navigate(f.page);
  // Focus values that follow Pimax Play are shown in the Pimax block instead of the hidden fields.
  const target = f.element.hidden ? (f.page === 'foveation' ? $('pimaxFovea') : f.element.closest('.card') ?? f.element) : f.element;
  for (let box = target.closest('details'); box; box = box.parentElement.closest('details')) box.open = true;
  // A folded list opens, so the found setting shows its programs.
  if (f.listSummary && f.input.hidden && !f.element.hidden) openList(f, true);
  target.scrollIntoView({block:'center'}); target.classList.remove('highlight'); void target.offsetWidth; target.classList.add('highlight');
  // A segmented choice takes focus on its selected option (the select under it is hidden).
  const focusTarget = f.segments ? f.segments.buttons.find(b => b.getAttribute('aria-checked') === 'true' && !b.disabled) || f.segments.buttons.find(b => !b.disabled) : f.input;
  if (!f.element.hidden && !f.input.disabled && focusTarget) { if (!['INPUT','BUTTON','SELECT','TEXTAREA'].includes(focusTarget.tagName)) focusTarget.tabIndex = -1; focusTarget.focus({preventScroll:true}); }
}
// ---- Updates and What's new ----------------------------------------------------------------------------------------
let updates = null, updateBannerHidden = false, updateError = null, updatePolling = false, updateNotesShown = null;
const shortVersion = v => String(v || '').split('-')[0];
const sizeText = bytes => bytes >= 1048576 ? (bytes/1048576).toFixed(1)+' MB' : Math.max(1,Math.round((bytes || 0)/1024))+' KB';
/** Release notes as plain text: paragraphs, headings and (nested) bullet lists built with text nodes, never innerHTML,
 *  because the notes are untrusted text from GitHub. **bold** shows bold; code marks and links show as their text. */
function notesFragment(markdown) {
  const out = document.createDocumentFragment(); let lists = [], para = null;
  const inline = (el, text) => {
    const plain = text.replace(/`([^`]*)`/g,'$1').replace(/\[([^\]]+)\]\([^)\s]*\)/g,'$1');
    plain.split(/\*\*(.+?)\*\*/).forEach((part,i) => { if (!part) return; if (i % 2) { const b = document.createElement('b'); b.textContent = part; el.append(b); } else el.append(document.createTextNode(part)); });
    return el;
  };
  for (const raw of String(markdown ?? '').replace(/\r/g,'').split('\n')) {
    const line = raw.replace(/\s+$/,'');
    if (!line.trim()) { lists = []; para = null; continue; }
    const bullet = /^(\s*)[-*+]\s+(.*)$/.exec(line), heading = /^#{1,6}\s+(.*)$/.exec(line);
    if (bullet) {
      let depth = Math.min(Math.floor(bullet[1].length / 2), 3); while (depth && !lists[depth-1]) depth--;
      lists.length = depth + 1; para = null;
      if (!lists[depth]) { const ul = document.createElement('ul'); (depth ? lists[depth-1].lastElementChild : out).append(ul); lists[depth] = ul; }
      lists[depth].append(inline(document.createElement('li'), bullet[2]));
    }
    else if (heading) { lists = []; para = null; out.append(inline(document.createElement('h3'), heading[1])); }
    else if (lists.length && /^\s/.test(line)) inline(lists[lists.length-1].lastElementChild, ' ' + line.trim());
    else if (para) inline(para, ' ' + line.trim());
    else { lists = []; para = inline(document.createElement('p'), line.trim()); out.append(para); }
  }
  return out;
}
/** The banner on the Overview, the dot on the rail's version, the Updates card (Diagnostics) and the open update dialog. */
function renderUpdates() {
  const u = updates, next = u?.update;
  $('updateBanner').hidden = !next || updateBannerHidden;
  if (next) $('updateBannerText').textContent = `DCS Control ${next.short} is available`;
  $('railUpdate').hidden = !next;
  $('appVersionButton').title = next ? `DCS Control ${next.short} is available. Click for what's new in this version.` : "What's new in this version";
  $('updateAuto').checked = Boolean(u?.enabled);
  const checked = u?.checkedAt ? ` Checked ${new Date(u.checkedAt).toLocaleString('en-GB')}.` : '';
  $('updateCheckStatus').textContent = !u ? '' : u.checking ? 'Checking GitHub…' : next ? `DCS Control ${next.short} is available.${checked}` : u.error ? u.error + checked
    : u.checkedAt ? `You have the latest version (${shortVersion(u.current)}).${checked}` : u.enabled ? 'Not checked yet.' : 'Automatic checks are off. Check now still asks GitHub.';
  $('updateCheckNow').disabled = Boolean(u?.checking || u?.installing);
  if ($('updateDialog').open) renderUpdateDialog();
}
function renderUpdateDialog() {
  const u = updates, next = u?.update;
  $('updateTitle').textContent = next ? `DCS Control ${next.short}` : 'DCS Control';
  $('updateMeta').textContent = next ? `Installed: ${shortVersion(u.current)}` + (next.published ? ` · released ${new Date(next.published).toLocaleDateString('en-GB')}` : '') + (next.bytes ? ` · ${sizeText(next.bytes)}` : '') : 'No update is available.';
  if (updateNotesShown !== (next?.version ?? null)) { $('updateNotes').replaceChildren(notesFragment(next ? next.notes || 'No release notes.' : '')); updateNotesShown = next?.version ?? null; }
  $('updatePortable').hidden = !next || !u.portable;
  if (next && u.portable) $('updatePortable').textContent = `This copy runs from ${u.appFolder}. The update installs DCS Control in ${u.installFolder}, with Start menu and desktop shortcuts; afterwards you can delete ${u.appFolder}. Your profiles and backups stay where they are.`;
  const d = u?.downloaded, progress = $('updateProgress');
  $('updateProgressRow').hidden = !u?.installing && !d;
  if (d?.total) progress.value = Math.min(1, d.done / d.total); else progress.removeAttribute('value');
  $('updateProgressText').textContent = d ? sizeText(d.done) + (d.total ? ' of ' + sizeText(d.total) : '') : 'Starting…';
  $('updateStatus').textContent = updateError || u?.message || (next && !next.canInstall ? 'This release has no installer to download: open its release page.' : '');
  $('updateStatus').classList.toggle('error', Boolean(updateError));
  $('updateInstall').disabled = !next?.canInstall || Boolean(u?.installing) || busy;
  // While downloading, closing the dialog cancels the download; once the setup runs there is nothing left to cancel.
  $('updateLater').textContent = u?.handedOff ? 'Close' : u?.installing ? 'Cancel download' : 'Later';
}
/** Asks the backend (at start: only when checks are on and the last check is over 6 hours old; Check now: always). */
async function checkUpdates(force) {
  if (updates) { updates.checking = true; renderUpdates(); }
  try { updates = await request('updateCheck',{force}); }
  catch (error) { if (updates) updates.error = error.message; }
  finally { if (updates) updates.checking = false; renderUpdates(); }
}
async function setUpdateChecks(on) {
  try { updates = await request('updateSettings',{checkForUpdates:on}); } catch (error) { showError(error); }
  renderUpdates();
}
function openUpdateDialog(start) {
  if (!updates?.update) return;
  updateError = null; renderUpdateDialog();
  if (!$('updateDialog').open) $('updateDialog').showModal();
  if (start && updates.update.canInstall) installUpdate();
}
/** Download, signature check and hand-over to the setup; the backend closes DCS Control once the setup runs. */
async function installUpdate() {
  if (!updates?.update?.canInstall || updates.installing) return;
  if (busy) { updateError = 'Finish the current operation first, then update.'; renderUpdateDialog(); return; }
  updateError = null; updates.installing = true; updates.downloaded = null; renderUpdateDialog();
  updatePolling = true;
  const poll = setInterval(() => request('updateStatus').then(r => { if (updatePolling) { updates = r; renderUpdates(); } }).catch(()=>{}), 300);
  try { const r = await request('updateInstall'); updatePolling = false; updates = r; }
  catch (error) { updatePolling = false; updateError = error.message; try { updates = await request('updateStatus'); } catch { updates.installing = false; } }
  finally { updatePolling = false; clearInterval(poll); renderUpdates(); renderUpdateDialog(); }
}
/** This version's section of the release notes: once at the first start after an update, and from the rail's version. */
async function showWhatsNew(startup) {
  try {
    const r = await request('whatsNew',{startup});
    if (!r.show) return false;
    $('whatsNewTitle').textContent = `What's new in DCS Control ${r.short}`;
    $('whatsNewNotes').replaceChildren(notesFragment(r.notes || 'This copy has no release notes. They are on the Releases page on GitHub.'));
    if (!$('whatsNewDialog').open) $('whatsNewDialog').showModal();
    return true;
  } catch (error) { if (!startup) showError(error); return false; }
}
function initializeUpdates() {
  $('appVersionButton').addEventListener('click',() => showWhatsNew(false));
  $('whatsNewOpen').addEventListener('click',() => showWhatsNew(false));
  $('whatsNewClose').addEventListener('click',() => $('whatsNewDialog').close());
  $('updateNotesOpen').addEventListener('click',() => openUpdateDialog(false));
  // From an installation Update starts at once; an unzipped copy first says where the update goes.
  $('updateOpen').addEventListener('click',() => openUpdateDialog(!updates?.portable));
  $('updateHide').addEventListener('click',() => { updateBannerHidden = true; renderUpdates(); });
  const leaveUpdate = () => { if (updates?.installing && !updates.handedOff) request('updateCancel').catch(()=>{}); };
  $('updateLater').addEventListener('click',() => { leaveUpdate(); $('updateDialog').close(); });
  $('updateDialog').addEventListener('cancel',leaveUpdate);
  $('updatePage').addEventListener('click',() => request('updatePage').catch(showError));
  $('updateInstall').addEventListener('click',() => installUpdate());
  $('updateCheckNow').addEventListener('click',() => checkUpdates(true));
  $('updateAuto').addEventListener('change',e => setUpdateChecks(e.target.checked));
}
async function initialize() {
  const groups = {engine:'Every pilot',foveation:'VR',setup:'Before you fly',support:'If something goes wrong'};
  pages.forEach(p => { if (groups[p[0]]) { const g = document.createElement('div'); g.className = 'rail-group'; g.id = 'railGroup-'+p[0]; g.textContent = groups[p[0]]; $('navigation').append(g); } const b = document.createElement('button'); b.id = 'nav-'+p[0]; b.className = 'nav-item'; b.title = PAGE_KEYS[p[0]] ? p[1]+' ('+PAGE_KEYS[p[0]].join(' ')+')' : p[1]; if (PAGE_KEYS[p[0]]) b.setAttribute('aria-keyshortcuts',PAGE_KEYS[p[0]].join('+').replace('Ctrl','Control')); b.setAttribute('aria-label',p[1]); b.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="${p[4]}"/></svg><div><b>${p[5] || p[1]}</b><small>${p[3]}</small></div>`; b.addEventListener('click',() => navigate(p[0])); $('navigation').append(b); });
  buildFields(); buildFeatureList(); nameMoreButtons(); navigate('overview');
  // Launch and Apply ask once before setting another program's dxgi.dll aside (ReShade, an overlay).
  // The page is busy from the click on, so a double click launches once and nothing waits on an idle page in between.
  ['launch','apply'].forEach(id => $(id).addEventListener('click',async () => {
    if (busy) return; setBusy(true);
    let asked;
    try { asked = await askDxgiFresh(id, true); } catch (error) { setBusy(false); showError(error); return; }
    setBusy(false);
    if (!asked) run(id).catch(()=>{});
  }));
  $('cancelDxgi').addEventListener('click',() => { pendingDxgiAction = null; $('dxgiDialog').close(); });
  // The choice is the profile's (Keep another program's dxgi.dll, DLSS page); the question is asked once per file.
  const decideDxgi = keep => {
    $('dxgiDialog').close(); const dxgi = state?.foreignDxgi; if (dxgi) try { localStorage.setItem('dxgiAck:'+dxgi.sha256,'1'); } catch {}
    if (profile.keepOtherDxgi !== keep) { profile.keepOtherDxgi = keep; showProfile(['keepOtherDxgi']); change(); }
    const action = pendingDxgiAction; pendingDxgiAction = null; if (action) run(action).catch(()=>{});
  };
  $('confirmDxgi').addEventListener('click',() => decideDxgi(true));
  $('asideDxgi').addEventListener('click',() => decideDxgi(false));
  [['importProfile','import'],['saveProfile','save'],['resetNeural','resetNeural'],['export','export'],['prepareSboys','prepareSboys'],['importSboys','importSboys'],['openSboys','openSboys'],['checkReadiness','checkReadiness']].forEach(([id,action]) => $(id).addEventListener('click',() => run(action).catch(()=>{})));
  $('refreshBoostPlan').addEventListener('click',() => refreshBoostPlan(0));
  $('refreshEngine').addEventListener('click',() => refreshEngine());
  $('engineSuite').addEventListener('click',() => engineAction('engineSuite'));
  $('engineReport').addEventListener('click',() => engineAction('engineOpen',{target:'report'}));
  $('engineLog').addEventListener('click',() => engineAction('engineOpen',{target:'log'}));
  $('refreshPupil').addEventListener('click',() => refreshPupil());
  $('refreshDlssFocus').addEventListener('click',() => refreshDlssFocus());
  $('diagnosticPackage').addEventListener('click',() => run('diagnosticPackage').catch(()=>{}));
  $('pupilLog').addEventListener('click',() => request('pupilOpenLog').catch(showError));
  $('searchOpen').addEventListener('click',openSearch); $('searchClose').addEventListener('click',() => $('searchDialog').close()); $('searchInput').addEventListener('input',renderSearch); $('searchInput').addEventListener('keydown',e => { if (e.key === 'Enter') { e.preventDefault(); revealSetting(searchResults[0]); } });
  $('refreshFlight').addEventListener('click',() => loadFlight(0)); $('resetApplied').addEventListener('click',resetToApplied);
  initializeUpdates(); initializeTour();
  document.addEventListener('keydown',e => { if (e.ctrlKey && (e.code === 'KeyK' || e.key.toLowerCase() === 'k')) { e.preventDefault(); if (!$('searchDialog').open) openSearch(); } if (!e.ctrlKey || e.altKey) return; const digit = /^Digit([0-9])$/.exec(e.code)?.[1] ?? (/^[0-9]$/.test(e.key) ? e.key : null); const key = e.shiftKey ? (e.code === 'KeyC' || e.key.toLowerCase() === 'c' ? 'C' : null) : digit; const target = key === null ? null : Object.keys(PAGE_KEYS).find(id => PAGE_KEYS[id].at(-1) === key && PAGE_KEYS[id].includes('Shift') === e.shiftKey); if (!target) return; e.preventDefault(); if (!$('nav-'+target).hidden) navigate(target); });
  try { sync(await request('ready')); loadFlight(); window.uiReady = true; showWhatsNew(true).then(shown => { if (!shown && !window.offlineFixture && !tourSeen(false)) openTour(); }); checkUpdates(false); }
  catch(e) { showError(e); }
}
initialize();
/** Takes a profile and DCS paths chosen elsewhere (guided setup) as the draft, exactly like editing them here. */
function adoptDraft(next) {
  profile = clone(next.profile); invalid.clear(); rememberProvider(profile); if (next.dcs !== undefined) dcs = next.dcs ?? ''; if (next.options !== undefined) options = next.options ?? '';
  fields.forEach(f => { if (f.path === 'dcs') setFieldValue(f, dcs); else if (f.path === 'options') setFieldValue(f, options); });
  showProfile(); change(); boostChanged();
}
// Test hooks exercise the same controls and command bridge; the fixture host never starts DCS or opens dialogs.
window.dcsUi = {request,run,setBusy,askDxgiFirst,askDxgiFresh,checkUpdates,installUpdate,setUpdateChecks,notesFragment,get updates(){return updates;},saveRuntime,forgetRuntime,adoptDraft,launchState,checkItems,loadFlight,resetToApplied,parseKey,keyLabel,get flight(){return flight;},get savedDraft(){return savedDraftJson;},refreshWorkflow:() => { renderOriginals(); updateWorkflow(); renderDashboard(); },runsWith,navigate,draft,sync,showProfile,invalidate:change,fields,withFeatures,featureState,featureNotes,featureSummary,deriveIdentity,FEATURES,ENGINE_PARTS,rereadPimax,refreshBoostPlan,renderBoostPlan,matchesApplied,effectiveFovea,revealSetting,get boostRequests(){return boostSequence;},get profile(){return profile;},get plan(){return plan;},get invalid(){return invalid;},get state(){return state;}};
// More / Less: shows or hides a setting's (or a card's) detail text; every page load starts with it folded.
document.addEventListener('click', e => {
  const b = e.target.closest('.more-toggle'); if (!b) return; const detail = $(b.getAttribute('aria-controls')); if (!detail) return;
  const open = b.getAttribute('aria-expanded') !== 'true'; b.setAttribute('aria-expanded',String(open)); detail.hidden = !open; b.textContent = open ? 'Less' : 'More';
});
document.addEventListener('click', e => { const reveal = e.target.closest('[data-reveal]'); if (reveal && !busy) revealSetting(fields.find(f => f.path === reveal.dataset.reveal)); });
document.addEventListener('click', e => { const stage = e.target.closest('.stage[data-page], [data-open-page]'); if (stage && !busy) navigate(stage.dataset.page || stage.dataset.openPage); });
document.addEventListener('click', e => {
  const link = e.target.closest('[data-reveal]'); if (!link || busy) return;
  const f = fields.find(x => x.path === link.dataset.reveal); revealSetting(f);
  if (link.dataset.browseAfter && f?.type === 'path' && !f.input.disabled) $('browse-'+f.path).click();
});
document.addEventListener('click', e => { if (e.target.closest('#rereadPimax') && !busy) rereadPimax(true); });
document.addEventListener('click', e => {
  const route = e.target.closest('#routeSwitch [data-route]');
  if (!route || busy) return;
  const target = route.dataset.route;
  if (target === 'Desktop') {
    if (profile.desktop) return;
    // Optimizations only: the VR settings stay in the profile and come back with the Pimax or Sboys route.
    profile.desktop = true;
  } else {
    if (!profile.desktop && target === profile.runtime) return;
    profile.desktop = false;
    // Switching route keeps every feature and setting; Pimax native Quad Views becomes the bundled provider on Sboys.
    if (target !== profile.runtime) { profile.runtime = target; profile.runtimeManifestPath = null; }
  }
  resolveFeatures(profile); Object.assign(profile, deriveIdentity(profile));
  change(); boostChanged();
});
let detectedProfile = null, detectedRefresh;
$('detectSetup').addEventListener('click', async () => {
  if (busy) return; setBusy(true);
  try {
    const result = await request('detect', draft()); detectedProfile = result.profile; detectedRefresh = result.headsetRefresh;
    if (state && result.activeRoute) { state.activeRoute = result.activeRoute; updateControls(); }
    $('detectActive').textContent = result.active;
    $('detectList').innerHTML = result.detected.length
      ? result.detected.map(d => `<div class="detect-row"><b>${escape(d.label)}</b><span>${escape(d.value)}</span><small>${escape(d.source)}</small></div>`).join('')
      : '<p class="fine">No settings to import were found.</p>';
    $('detectNotes').innerHTML = result.notes.map(n => `<p>${escape(n)}</p>`).join('');
    $('confirmDetect').disabled = result.detected.length === 0;
    $('detectDialog').showModal();
  } catch (error) { showError(error); } finally { setBusy(false); }
});
$('cancelDetect').addEventListener('click', () => $('detectDialog').close());
$('confirmDetect').addEventListener('click', () => {
  $('detectDialog').close(); if (!detectedProfile) return;
  if (state && detectedRefresh !== undefined) state.headsetRefresh = detectedRefresh;
  profile = clone(detectedProfile); detectedProfile = null; invalid.clear(); rememberProvider(profile); showProfile(); change(); boostChanged();
});
// Orange fill on slider tracks, as in the launcher.
function paintRange(range) { const min = Number(range.min), max = Number(range.max), value = Number(range.value); range.style.setProperty('--fill', (max > min ? (value - min) / (max - min) * 100 : 0) + '%'); }
document.addEventListener('input', e => { if (e.target.type === 'range') paintRange(e.target); });
// Radio-group keyboard behaviour for the route switch: arrows move the selection.
$('routeSwitch').addEventListener('keydown', e => {
  if (!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(e.key)) return;
  e.preventDefault();
  const buttons = [...$('routeSwitch').querySelectorAll('[data-route]')];
  const step = e.key === 'ArrowLeft' || e.key === 'ArrowUp' ? -1 : 1;
  const next = buttons[(buttons.indexOf(document.activeElement) + step + buttons.length) % buttons.length];
  next.focus(); next.click();
});
// Review files builds the read-only file list for the current draft; Back to stock DCS opens Recovery.
['reviewPlan','reviewPlanHere'].forEach(id => $(id).addEventListener('click', () => run('preview').catch(()=>{})));
$('openRecovery').addEventListener('click', () => navigate('recovery'));
document.querySelector('.panel-top').addEventListener('scroll', updatePanelScroll, {passive:true});
window.addEventListener('resize', updatePanelScroll);
// Title-bar buttons: the window has no Windows frame, so the page provides them.
document.querySelectorAll('[data-window]').forEach(button => button.addEventListener('click',() => request('window',{op:button.dataset.window}).then(r => { const max = document.querySelector('[data-window="maximize"]'); max.setAttribute('aria-label',r.maximized ? 'Restore' : 'Maximize'); max.title = r.maximized ? 'Restore' : 'Maximize'; }).catch(showError)));

'use strict';
const $ = id => document.getElementById(id);
const pages = [
  ['overview','Overview','What runs when you press Launch DCS, and how your last flight went.','Status at a glance','M3.5 16.5a8.5 8.5 0 1 1 17 0 M12 16.5l4.2-5.8 M6.3 11.4l1.1.7 M12 8.2v1.3 M17.7 11.4l-1.1.7 M13.3 16.5a1.3 1.3 0 1 1-2.6 0 1.3 1.3 0 0 1 2.6 0Z M5 20h14'],
  ['engine','Engine Optimizations','CPU optimizations inside DCS for every pilot, on a monitor or in VR: less work on its render and model threads, the same image.','CPU work inside DCS','M12 8.8a3.2 3.2 0 1 0 0 6.4 3.2 3.2 0 0 0 0-6.4Z M12 3v2.6 M12 18.4V21 M3 12h2.6 M18.4 12H21 M5.6 5.6l1.9 1.9 M16.5 16.5l1.9 1.9 M5.6 18.4l1.9-1.9 M16.5 7.5l1.9-1.9','Engine'],
  ['boost','CPU Boost','Process priority, CPU cores and the prefetch fix, plus helpers that free VRAM and lighten the monitor while DCS runs.','CPU, VRAM & monitor','M6 6h12v12H6Z M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3 M12.8 8.5 10.2 12.3h3.6l-2.6 3.7'],
  ['foveation','Quad Views','Foveated rendering: high detail where you look, less in the periphery.','Foveated rendering','M3 8V5a2 2 0 0 1 2-2h3 M16 3h3a2 2 0 0 1 2 2v3 M21 16v3a2 2 0 0 1-2 2h-3 M8 21H5a2 2 0 0 1-2-2v-3 M6 12c1.6-2.7 3.6-4 6-4s4.4 1.3 6 4c-1.6 2.7-3.6 4-6 4s-4.4-1.3-6-4Z M14 12a2 2 0 1 1-4 0 2 2 0 0 1 4 0Z'],
  ['dlss','DLSS 5','NVIDIA DLSS 5 on the image, and Foveated Super Resolution.','Neural rendering','M10 3c.6 3.8 2.2 5.4 6 6-3.8.6-5.4 2.2-6 6-.6-3.8-2.2-5.4-6-6 3.8-.6 5.4-2.2 6-6Z M18 14c.3 1.8 1.1 2.6 3 3-1.9.4-2.7 1.2-3 3-.3-1.8-1.1-2.6-3-3 1.9-.4 2.7-1.2 3-3Z'],
  ['framegen','Framegen','OFXR frame generation, the DCS frame limit and in-headset diagnostics.','Generation & limit','M3 10h11v10H3Z M6.5 10V6.5h11v10H14 M10 6.5V3h11v10h-3.5'],
  ['pupil','Pupil shift','Experimental. Each eye is rendered from where its pupil is as you look around, not from one fixed eye point.','Experimental · eye position','M2.5 12c2.2-3.7 5.4-5.5 9.5-5.5s7.3 1.8 9.5 5.5c-2.2 3.7-5.4 5.5-9.5 5.5S4.7 15.7 2.5 12Z M14.5 12a2.5 2.5 0 1 1-5 0 2.5 2.5 0 0 1 5 0Z M17 3.5l2.5 2.5L17 8.5 M19.5 6h-5'],
  ['setup','DCS & headset','Where DCS is installed, its launcher and, for VR, the headset software.','Paths & launcher','M3 9a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2h-4.2l-1.8-2.4h-2L9.2 17H5a2 2 0 0 1-2-2Z M9.5 12a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0Z M17.5 12a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0Z'],
  ['diagnostics','Checks','What to fix before launching, what to check yourself, and the files Launch DCS writes.','Checks & reports','M9 3.5h6v3H9Z M9 5H6.5A1.5 1.5 0 0 0 5 6.5v13A1.5 1.5 0 0 0 6.5 21h11a1.5 1.5 0 0 0 1.5-1.5v-13A1.5 1.5 0 0 0 17.5 5H15 M8.5 13.5l2.5 2.5 4.5-5'],
  ['support','Diagnostics','Why something in the headset does not work as expected, and a diagnostic package to attach to a report.','Problems & support','M12 3.5a8.5 8.5 0 1 0 0 17 8.5 8.5 0 0 0 0-17Z M9.6 9.4a2.5 2.5 0 1 1 3.4 2.3c-.6.3-1 .8-1 1.5v.6 M12 16.4v.2'],
  ['recovery','Stock DCS','Back to stock DCS puts DCS back exactly as it was before DCS Control: every file and setting it changed, in one step.','Undo every change','M4.5 12a7.5 7.5 0 1 0 2.2-5.3L4.5 9 M4.5 4.5V9H9 M12 8.5v3.5l2.5 1.8']
];
/** Pages of the VR features, hidden in Optimizations only (no VR). */
const VR_PAGES = ['foveation','dlss','framegen','pupil'];
// The feature checklist in the right panel; each feature has its own page. First what every pilot can use (monitor or any
// headset), then the VR features for Pimax (and Sboys).
const FEATURES = [
  ['engine','Engine Optimizations','CPU work inside DCS','engine'],
  ['boost','CPU Boost','CPU scheduling','boost'],
  ['quad','Quad Views','foveated rendering · VR','foveation'],
  ['dlss','DLSS 5','neural rendering · VR','dlss'],
  ['framegen','Frame generation','OFXR · VR','framegen']
];
/** The engine optimizations a profile turns on (Engine Optimizations page). */
const ENGINE_PARTS = ['engineShaderTimeCache','enginePartitionBoost','engineCostWeights','engineShadowInstancing','engineFrameHeap','engineTaskQueueClock','engineModelAllocator','engineTextureDedupe','engineEffectBufferSkip','enginePlainCounter','engineStateFilter','engineShadowRecorder','engineGBufferRecorder'];
let state, profile, dcs = '', options = '', page = 'overview', busy = false, plan = null, draftChanged = false;
let sequence = 0, toastTimer, searchResults = [], lastProvider = 'QuadViewsFoveated', lastFrameGen = 'Nvidia', savedDraftJson = null, draftTimer, flight = null;
const pending = new Map(), fields = [], invalid = new Set();
const clone = value => JSON.parse(JSON.stringify(value));
const NO_TIMEOUT = new Set(['browse','save','import','export','importSboys','prepareSboys','diagnosticPackage','launch','apply','restore','grantWriteAccess']);
const escape = value => String(value ?? '').replace(/[&<>"']/g,c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const empty = value => value.trim() || null;
const num = value => String(Number(Number(value).toFixed(4)));
/** A profile's own focus values in Pimax Play's Quick units, converted exactly like Pimax Quick (Pimax stores 4 decimals):
 *  Horizontal/Vertical FOV q leaves a focus of 1 − q of the view; Center Resolution c is a pixel density of
 *  1 + (c − 100) / 200 (125% → 1.125); Peripheral Resolution r is a factor of (r − 1) / 99 (20% → 0.1919). */
const fixed4 = v => Number(v.toFixed(4));
const PIMAX = {
  quickFromShare: s => (1 - s) * 100, shareFromQuick: q => Number((1 - fixed4(q * .01)).toFixed(10)),
  centerFromDensity: d => 100 + (d - 1) * 200, densityFromCenter: c => Number((1 + fixed4((c - 100) / 100) * .5).toFixed(10)),
  peripheralPercent: r => 1 + r * 99, peripheryFromPercent: v => fixed4((v - 1) / 99)
};
/** Shown like Pimax Play shows its sliders: whole percent, rounded half up. Stored values change only when edited. */
const whole = v => Math.round(v);
const profileQuick = p => `${whole(PIMAX.quickFromShare(p.foveaWidth))}% × ${whole(PIMAX.quickFromShare(p.foveaHeight))}% · ${whole(PIMAX.centerFromDensity(p.quadFocusScale))}% · ${whole(PIMAX.peripheralPercent(p.peripheralScale))}%`;
function request(action, data = {}) {
  return new Promise((resolve,reject) => {
    if (!window.chrome?.webview) { reject(new Error('Open this interface in DCS Control to connect to its local services.')); return; }
    const id = String(++sequence);
    // A file dialog, a Windows prompt or a download waits for the pilot as long as it takes.
    const timer = NO_TIMEOUT.has(action) ? null : setTimeout(() => { pending.delete(id); reject(new Error('The operation timed out. Rescan this PC before retrying.')); },120000);
    pending.set(id,{resolve,reject,timer}); window.chrome.webview.postMessage({id,action,data});
  });
}
window.chrome?.webview?.addEventListener('message',e => {
  const item = pending.get(e.data.id); if (!item) return; pending.delete(e.data.id); clearTimeout(item.timer);
  e.data.ok ? item.resolve(e.data.data) : item.reject(new Error(e.data.error));
});
const draft = extras => ({profile:clone(profile),dcs,options,...extras});
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
  ['importProfile','saveProfile'].forEach(id => $(id).disabled = value); updateWorkflow();
}
const WORKING = {launch:'Bringing the profile up to date and starting DCS…', apply:'Applying the profile without launching…', preview:'Listing the files Launch DCS writes…', restore:'Putting the original files back…'};
async function run(action, extras = {}) {
  if (invalid.size && ['preview','apply','launch','save','resetNeural','export','checkReadiness'].includes(action)) { const error = new Error('Correct the marked numeric values first.'); showError(error); throw error; }
  // A second click while a command runs is ignored: Launch and Apply never run twice.
  if (busy) return; setBusy(true); $('statusTitle').textContent = 'Working'; $('status').textContent = WORKING[action] || 'Checking the local configuration…';
  if (action === 'preview' || action === 'apply' || action === 'launch') plan = null;
  try { const result = await request(action,draft(extras)); sync(result); if (action === 'preview') showPlan(); return result; }
  catch(error) { showError(error); throw error; }
  finally { setBusy(false); updateControls(); }
}
/** Review files: the read-only list of what Launch DCS writes for this draft. */
function showPlan() { navigate('diagnostics'); $('planCard').scrollIntoView({block:'start'}); }
function setFieldValue(f, value) {
  if (f.map) value = f.map.get(profile);
  if (f.type === 'hotkey') { f.input.value = value ?? ''; showHotkey(f); f.element.classList.remove('invalid'); return; }
  if (f.type === 'toggle') f.input.checked = value; else if (f.type === 'list') f.input.value = (value || []).join('\n'); else f.input.value = value ?? '';
  if (f.range) { f.range.value = value; paintRange(f.range); } f.element.classList.remove('invalid'); f.input.setAttribute('aria-invalid','false'); f.error && (f.error.hidden = true);
}
function sync(next) {
  state = next; if (next.version) $('appVersion').textContent = next.version.split('-')[0]; profile = clone(next.profile); dcs = next.dcs ?? ''; options = next.options ?? ''; plan = next.plan; draftChanged = false; invalid.clear();
  // A provider forced by DLSS or the Sboys route is not a choice: the remembered provider stays as it is.
  if (profile.quadViews === 'PimaxNative' || (profile.quadViews === 'QuadViewsFoveated' && nativeAllowed(profile))) lastProvider = profile.quadViews;
  if (profile.frameGen !== 'Off') lastFrameGen = profile.frameGen;
  fields.forEach(f => setFieldValue(f, f.path === 'dcs' ? dcs : f.path === 'options' ? options : profile[f.path]));
  // The first reply is the draft already on disk (or the applied profile); later replies are saved when they differ.
  if (savedDraftJson === null) savedDraftJson = JSON.stringify(draft()); else saveDraftSoon();
  $('openSboys').disabled = !next.sboysReady;
  $('statusDot').classList.remove('error'); $('status').textContent = next.status + (next.foveaChanged ? ' ' + next.foveaChanged : '');
  renderDiagnostics(); renderOriginals(); updateControls();
}
// ---- Features: what the checklist turns on, and what follows from it -------------------------------------------
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
    const n = ENGINE_PARTS.filter(k => p[k]).length;
    return `${n} of ${ENGINE_PARTS.length} on · stays on outside the app`;
  }
  if (key === 'quad') {
    if (p.quadViews === 'None') return 'Off · normal stereo rendering';
    if (p.quadViews === 'PimaxNative') return 'Pimax native · ' + (state?.pimax?.found ? 'Pimax ' + state.pimax.short : 'uses Pimax Play settings');
    const e = effectiveFovea(p), gaze = p.gaze === 'EyeTracked' ? 'eye tracked' : 'fixed';
    if (p.quadViewsLayerDirectory) return 'Alternative provider · '+gaze;
    // In Pimax Play's units: "Pimax Quick 33% × 33% · 125% · 20%", or this profile's values in the same Quick units.
    if (pimaxSource(p)) return `Bundled · ${gaze} · ` + (state?.pimax?.found ? 'from Pimax Play\'s settings: ' + state.pimax.short : 'Pimax Play not found, profile values');
    return `Bundled · ${gaze} · profile ${profileQuick(p)}`;
  }
  if (key === 'dlss') {
    if (!p.neuralRendering) return foveatedSr(p) ? 'Foveated Super Resolution on · DLSS 5 off' : 'Off · original image';
    const area = p.quadFocusAdapter ? (p.neuralFocusArea < 100 ? 'Central '+p.neuralFocusArea+'%' : 'Whole focus area') : 'Whole view';
    return [area, p.neuralStyle, foveatedSr(p) ? 'Foveated SR' : null, p.neuralRuntimePath || state?.savedRuntime ? null : 'runtime file missing'].filter(Boolean).join(' · ');
  }
  if (key === 'framegen') {
    if (p.frameGen === 'Off') return 'Off · rendered frames only';
    const factor = p.frameGenFactor === 0 ? 'Auto 2×/3×' : p.frameGenFactor+'×';
    return `${p.frameGen === 'Nvidia' ? 'NVIDIA' : 'FidelityFX'} · ${factor} · renders ${Number((refreshHz(p)/(p.frameGenFactor === 3 ? 3 : 2)).toFixed(1))} FPS`;
  }
  // The flight helpers on the same page run with or without CPU Boost.
  const vr = !p.desktop;
  const helpers = [p.freeVram ? 'VRAM freed' : null, vr && p.pauseTobiiDesktop ? 'desktop Tobii paused' : null, vr && p.smallDcsWindow ? 'small DCS window' : null, vr && p.lowerMonitor ? `monitor ${p.flightDisplayWidth}×${p.flightDisplayHeight}` : null];
  if (!p.cpuBoost) return ['Off · Windows scheduling', ...helpers].filter(Boolean).join(' · ');
  const apps = p.boostMoveBackgroundApps ? (p.boostBackgroundApps || []).length : 0;
  return [PRIORITY[p.boostDcsPriority] || p.boostDcsPriority, apps ? apps+' apps moved' : null, p.boostPrefetch === 'Skip' ? 'prefetch fix' : p.boostPrefetch === 'Observe' ? 'prefetch observed' : null, ...helpers].filter(Boolean).join(' · ');
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
  if (p.neuralRendering && !p.neuralRuntimePath && !state?.savedRuntime) notes.dlss.push(['needs','Needs nvngx_dlssnr.dll.','neuralRuntimePath','Select file']);
  if ((p.cpuBoost && p.boostElevated) || (p.pauseTobiiDesktop && !p.desktop)) notes.boost.push(['info','Asks for admin rights (UAC) at launch.']);
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
  profile = clone(state.appliedDraft); invalid.clear();
  // The DCS install and options.lua the applied profile was written to.
  if (state.appliedDcs) dcs = state.appliedDcs; if (state.appliedOptions) options = state.appliedOptions;
  fields.forEach(f => { if (f.path === 'dcs') setFieldValue(f, dcs); else if (f.path === 'options') setFieldValue(f, options); });
  showProfile(); change(); boostChanged();
  $('status').textContent = 'Back to the applied profile.';
}
function change() {
  resolveFeatures(profile);
  // Pimax native comes back once nothing forces the bundled provider any more (DLSS off, Pimax route again).
  if (profile.quadViews === 'QuadViewsFoveated' && lastProvider === 'PimaxNative' && nativeAllowed(profile) && !profile.quadViewsLayerDirectory) profile.quadViews = 'PimaxNative';
  Object.assign(profile, deriveIdentity(profile));
  if (profile.frameGen !== 'Off') lastFrameGen = profile.frameGen;
  showProfile(FEATURE_FIELDS);
  plan = null; draftChanged = true; $('statusDot').classList.remove('error');
  if (state) { state.readiness = null; renderEdits(); }
  $('status').textContent = 'Profile edited. Launch DCS applies it; Review files lists what it writes.';
  request('changed').catch(showError); updateControls(); saveDraftSoon();
  if (page === 'engine') renderEngine();
  if (page === 'pupil') { renderPupilNeeds(); renderPupil(); }
}
/** "What happens at launch" depends only on the Boost settings and the route: refreshed for those edits, not every edit. */
function boostChanged() { if (page === 'boost') refreshBoostPlan(); }
// Fields the checklist and the route switch can change; other fields only change through their own input.
const FEATURE_FIELDS = ['quadViews','neuralRendering','foveatedDlss','frameGen','cpuBoost','engineOptimizations','pauseTobiiDesktop','runtimeManifestPath'];
/** Shows profile values in the fields (all, or only `paths`), never overwriting an entry the user is correcting. */
function showProfile(paths) {
  fields.forEach(f => { if (f.path === 'dcs' || f.path === 'options' || invalid.has(f.path) || (paths && !paths.includes(f.path))) return; setFieldValue(f, profile[f.path]); });
}
function card(parent,title,subtitle) {
  const panel = document.createElement('div'); panel.className = 'card';
  panel.innerHTML = `<div class="card-heading"><div><h2>${title}</h2><p class="fine">${subtitle}</p></div></div>`; $(parent).append(panel); return panel;
}
/** A closed disclosure inside a card for settings that are real but rarely changed. Search and links open it. */
function disclosure(panel,title,subtitle) {
  const box = document.createElement('details'); box.className = 'advanced';
  box.innerHTML = `<summary>${title}<small>${subtitle}</small></summary>`; panel.append(box); return box;
}
/** `map` shows a profile value in other units ({get(profile), set(profile,value)}); the profile keeps its own value until edited. */
function field(panel,path,title,description,type,values,condition,unit='',hide=null,map=null) {
  const element = document.createElement('div'); element.className = 'setting'; element.id = 'setting-'+path;
  const id = 'input-'+path; let inputMarkup;
  if (type === 'toggle') inputMarkup = `<div class="switch-row"><label for="${id}"><span class="setting-label">${title}</span><p>${description}</p></label><input id="${id}" class="switch" type="checkbox" role="switch"></div>`;
  else {
    inputMarkup = `<label class="setting-label" for="${id}">${title}</label><p id="help-${path}">${description}</p>`;
    if (type === 'select') inputMarkup += `<select id="${id}" aria-describedby="help-${path}">${values.map(([v,t]) => `<option value="${v}">${t}</option>`).join('')}</select>`;
    if (type === 'number') inputMarkup += `<div class="setting-number"><input id="range-${path}" type="range" min="${values[0]}" max="${values[1]}" step="${values[2]}" aria-label="${title} slider"><input id="${id}" class="number-input" type="text" inputmode="decimal" autocomplete="off" spellcheck="false" min="${values[0]}" max="${values[1]}" aria-describedby="help-${path} error-${path}"><span class="number-unit">${unit}</span></div><span id="error-${path}" class="input-error" hidden></span>`;
    if (type === 'path') inputMarkup += `<div class="path-row"><input id="${id}" type="text" spellcheck="false" aria-describedby="help-${path}"><button class="button secondary" id="browse-${path}">Browse</button></div>`;
    if (type === 'list') inputMarkup += `<textarea id="${id}" class="list-input" rows="5" spellcheck="false" aria-describedby="help-${path}"></textarea>`;
    if (type === 'hotkey') inputMarkup += `<div class="hotkey-row"><button type="button" id="${id}" class="hotkey-capture" aria-describedby="help-${path} keynote-${path}"></button><span id="keynote-${path}" class="hotkey-note" role="status"></span></div>`;
  }
  element.innerHTML = inputMarkup; panel.append(element); const input = $(id), range = $('range-'+path), error = $('error-'+path);
  const f = {path,prop:map?.prop ?? path,title,description,type,values,condition,unit,element,input,range,error,hide,map,page:panel.closest('.page').id.slice(5)}; fields.push(f);
  if (type === 'hotkey') captureHotkey(f);
  input.addEventListener(type === 'toggle' || type === 'select' || type === 'hotkey' ? 'change' : 'input', () => {
    let value = type === 'toggle' ? input.checked : input.value;
    if (type === 'number') {
      // "0,5" (a decimal comma, typed on many European keyboards and number pads) reads as 0.5.
      const n = Number(String(value).trim().replace(',', '.')), valid = String(value).trim() !== '' && Number.isFinite(n) && n >= values[0] && n <= values[1];
      element.classList.toggle('invalid',!valid); input.setAttribute('aria-invalid',String(!valid)); error.hidden = valid;
      if (!valid) { invalid.add(path); error.textContent = `Enter a value from ${values[0]} to ${values[1]}.`; change(); return; }
      invalid.delete(path); value = n; range.value = n; paintRange(range);
    } else if (type === 'select' && typeof values[0][0] === 'number') value = Number(value);
    else if (type === 'list') value = value.split(/[\n,;]/).map(s => s.trim()).filter(Boolean);
    if (path === 'dcs') dcs = value; else if (path === 'options') options = value; else if (map) map.set(profile,value); else profile[path] = type === 'path' ? empty(value) : value;
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
const LEGACY_KEYS = {'Off':[0,0],'Ctrl+Shift+F12':[123,5],'Alt+Shift+F12':[123,6],'Ctrl+Alt+Shift+F10':[121,7],'Ctrl+Alt+Shift+F12':[123,7],'Ctrl+Alt+Shift+Home':[36,7],'Ctrl+Alt+Shift+End':[35,7],'Ctrl+Alt+Shift+Insert':[45,7]};
const KEY_NAMES = {8:'Backspace',9:'Tab',13:'Enter',19:'Pause',20:'Caps Lock',27:'Esc',32:'Space',33:'Page Up',34:'Page Down',35:'End',36:'Home',37:'Left',38:'Up',39:'Right',40:'Down',44:'Print Screen',45:'Insert',46:'Delete',106:'Num *',107:'Num +',109:'Num -',110:'Num .',111:'Num /',144:'Num Lock',145:'Scroll Lock',186:';',187:'=',188:',',189:'-',190:'.',191:'/',192:'`',219:'[',220:'\\',221:']',222:"'",226:'\\ (102nd key)'};
function parseKey(value) {
  if (value == null) return null;
  const legacy = Object.keys(LEGACY_KEYS).find(k => k.toLowerCase() === String(value).trim().toLowerCase()); if (legacy) return LEGACY_KEYS[legacy];
  const m = /^(\d{1,3}):([0-7])$/.exec(String(value).trim()); if (!m) return null;
  const vk = Number(m[1]), mods = Number(m[2]); return vk <= 254 && (vk || !mods) ? [vk,mods] : null;
}
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
  neuralToggleKey:{others:['diagnosticOverlayKey','engineToggleKey','pupilShiftToggleKey'],used:p => p.neuralRendering,name:'DLSS 5 toggle',fallback:'Ctrl+Shift+F12'},
  diagnosticOverlayKey:{others:['neuralToggleKey','engineToggleKey','pupilShiftToggleKey'],used:p => p.frameGen !== 'Off',name:'diagnostic panel',fallback:'Alt+Shift+F12'},
  engineToggleKey:{others:['neuralToggleKey','diagnosticOverlayKey','pupilShiftToggleKey'],used:p => p.engineOptimizations,name:'engine optimizations switch',fallback:'122:6'},
  pupilShiftToggleKey:{others:['neuralToggleKey','diagnosticOverlayKey','engineToggleKey'],used:p => usesPupilShift(p),name:'pupil shift switch',fallback:'121:5'}
};
/** Ctrl+Alt+F8 to F12 and Ctrl+Alt+Page Up/Down belong to the engine optimizations module itself. */
const engineReserved = ([vk,mods]) => mods === 3 && ((vk >= 0x77 && vk <= 0x7B) || vk === 33 || vk === 34);
const isDcsDefault = k => Boolean(k && k[0] && state?.dcsDefaultKeys?.includes(keyCode(k)));
/** The recorded key, and a note when DCS binds it by default (it still works; DCS reacts too). */
function showHotkey(f, message, kind) {
  if (!f.input.classList.contains('capturing')) f.input.innerHTML = `<kbd>${escape(keyLabel(parseKey(f.input.value) ?? parseKey(HOTKEYS[f.path].fallback)))}</kbd>`;
  const k = parseKey(f.input.value), note = $('keynote-'+f.path);
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
    if (next[0] && engineReserved(next)) { showHotkey(f,`${keyLabel(next)} is used by the engine optimizations' test suite.`,'error'); prompt(0); return; }
    const current = parseKey(button.value);
    stop();
    // The same combination keeps the profile's own spelling (an earlier version's label stays as it is).
    if (!current || keyCode(current) !== keyCode(next)) { button.value = next[0] ? keyCode(next) : 'Off'; button.dispatchEvent(new Event('change',{bubbles:true})); }
    showHotkey(f);
  }
  button.addEventListener('click',() => {
    if (busy || button.disabled) return;
    if (button.classList.contains('capturing')) { stop(); return; }
    button.classList.add('capturing'); button.setAttribute('aria-pressed','true'); prompt(0);
    $('keynote-'+f.path).textContent = 'Esc cancels · Backspace clears'; $('keynote-'+f.path).className = 'hotkey-note';
    window.addEventListener('keydown',onKey,true); window.addEventListener('keyup',onUp,true);
  });
  button.addEventListener('blur',() => { if (button.classList.contains('capturing')) stop(); });
}
const nr = p => p.neuralRendering, flow = p => p.frameGen === 'Nvidia', quad = p => p.quadViews === 'QuadViewsFoveated' && !p.quadViewsLayerDirectory;
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
  // Quad Views page: provider and focus movement first, then where the focus area comes from.
  let c = card('foveationControls','Foveated rendering','DCS renders a sharp focus area and a lower-resolution periphery.');
  field(c,'quadViews','Quad Views provider','Bundled works with DLSS 5 and frame generation. Pimax native uses Pimax Play\'s own Quad Views: Pimax route only, without DLSS 5.','select',[['None','Off · normal stereo'],['QuadViewsFoveated','Bundled Quad Views (recommended)'],['PimaxNative','Pimax native Quad Views']]);
  field(c,'gaze','Focus movement','Follow your eyes, or keep the focus area centered.','select',[['EyeTracked','Eye tracked'],['Fixed','Fixed (centered)']],p => p.quadViews !== 'None');
  field(c,'quadTurbo','Turbo mode','DCS starts the next frame while the headset still shows the previous one. Can help when the CPU limits FPS.','toggle',null,quad);
  let more = disclosure(c,'Alternative provider','Only to test another Quad Views build.');
  field(more,'quadViewsLayerDirectory','Alternative Quad Views provider','Folder of another Quad Views build. Leave empty for the bundled one, which DLSS 5 needs.','path',null,p => p.quadViews === 'QuadViewsFoveated');
  c = card('focusControls','Focus area','Size and sharpness of the high-detail area, in Pimax Play\'s own units.');
  field(c,'foveaSource','Focus area source','From Pimax Play\'s settings: the Quad View values set in Pimax Play, read again at every launch, on the Pimax and the Sboys route alike; nothing is written to Pimax. This profile: set them below, in Pimax Play\'s Quick units.','select',[['PimaxPlay','From Pimax Play\'s settings'],['Profile','This profile']],quad);
  const pimaxBox = document.createElement('div'); pimaxBox.id = 'pimaxFovea'; pimaxBox.className = 'pimax-fovea'; pimaxBox.hidden = true; c.append(pimaxBox);
  // This profile's own focus with bundled Quad Views, edited like Pimax Play's Quick sliders and converted exactly as Pimax converts them.
  const ownFocusHidden = p => p.quadViews !== 'QuadViewsFoveated' || pimaxSource(p);
  field(c,'quadFocusScale','Center resolution','As in Pimax Play: 125% renders the focus at a 1.125× pixel density. Above 100% adds detail and GPU cost.','number',[100,200,1],quad,'%',ownFocusHidden,{prop:'quadFocusScale',get:p => whole(PIMAX.centerFromDensity(p.quadFocusScale)),set:(p,v) => p.quadFocusScale = PIMAX.densityFromCenter(v)});
  field(c,'pimaxPeripheral','Peripheral resolution','As in Pimax Play: 20% renders the periphery at 0.1919 of full resolution. Lower values save GPU outside the focus; bundled Quad Views needs at least 16%.','number',[16,100,1],quad,'%',ownFocusHidden,{prop:'peripheralScale',get:p => whole(PIMAX.peripheralPercent(p.peripheralScale)),set:(p,v) => p.peripheralScale = PIMAX.peripheryFromPercent(v)});
  field(c,'pimaxHorizontalFov','Horizontal FOV','As Pimax Play\'s Quick Horizontal FOV: the share of the view left outside the focus, so 33% renders a focus 67% of the view wide.','number',[5,90,1],quad,'%',ownFocusHidden,{prop:'foveaWidth',get:p => whole(PIMAX.quickFromShare(p.foveaWidth)),set:(p,v) => p.foveaWidth = PIMAX.shareFromQuick(v)});
  field(c,'pimaxVerticalFov','Vertical FOV','As Pimax Play\'s Quick Vertical FOV: 33% renders a focus 67% of the view tall.','number',[5,90,1],quad,'%',ownFocusHidden,{prop:'foveaHeight',get:p => whole(PIMAX.quickFromShare(p.foveaHeight)),set:(p,v) => p.foveaHeight = PIMAX.shareFromQuick(v)});
  // Stereo Cheeky (DLSS 5 or Foveated Super Resolution without Quad Views) sizes its own focus as fractions of the view.
  const stereoHidden = p => p.quadViews !== 'None';
  field(c,'foveaWidth','Focus width','Without Quad Views: horizontal share of the view in high detail.','number',[.1,1,.01],coverageOwned,'ratio',stereoHidden);
  field(c,'foveaHeight','Focus height','Without Quad Views: vertical share of the view in high detail.','number',[.1,1,.01],coverageOwned,'ratio',stereoHidden);
  field(c,'peripheralScale','Peripheral resolution','Foveated Super Resolution without Quad Views: lower values save GPU outside the focus area.','number',[.15,1,.01],foveatedSr,'ratio',stereoHidden);
  field(c,'quadSharpening','Focus sharpening','Extra sharpening of the focus area.','number',[0,1,.01],quad,'ratio');
  field(c,'quadPeripheryContrast','Periphery contrast','Restores the local contrast the periphery loses when Quad Views enlarges it, so it reads less blurred next to the focus area. Runs at the periphery\u2019s own low resolution: a fraction of a millisecond. 0 turns it off; raise it slowly, too much makes the edges of the periphery shimmer.','number',[0,1,.05],quad,'ratio');
  field(c,'quadSharpenTaper','Sharpening fades at the edge','The focus sharpening fades out over the same band as the focus area, so detail steps down gradually into the periphery and the edge is harder to see. Almost no GPU cost.','toggle',null,quad);
  field(c,'quadRoundFocus','Round focus area','On: the sharp area is a circle. Off: a rectangle with slightly rounded corners, which keeps more sharp area at the corners. Both fade into the periphery down to nothing over Focus edge blending.','toggle',null,quad);
  const tracked = p => quad(p) && p.gaze === 'EyeTracked';
  field(c,'quadSaccadeLead','Focus ahead of fast eye movements','When your eyes jump, the sharp area reaches ahead along the movement for that moment, so with frame generation you land in it instead of waiting a frame or two for it. No GPU cost: the focus keeps its pixels and is briefly a little less dense while the eyes move.','toggle',null,tracked);
  field(c,'quadGazeStabilize','Steady focus while looking','Keeps the sharp area still while your eyes rest (movements under half a degree), so eye-tracker noise does not make its edge shimmer. Real eye movements are followed at once.','toggle',null,tracked);
  field(c,'quadEdgeBlend','Focus edge blending','How wide the focus area fades into the periphery: wider is softer, with a little less fully sharp area. Following Pimax Play, 0 (a sharp edge) when its Transition Mode is Off.','number',[0,.5,.01],quad,'ratio');
  // DLSS 5 page: on/off and the runtime file first, Foveated Super Resolution (stereo only) on its own, then the image controls.
  c = card('dlssControls','DLSS 5','Neural rendering with your own NVIDIA runtime file, which is not included with this app.');
  field(c,'neuralRendering','DLSS 5','Runs NVIDIA DLSS 5 on the image. With Quad Views, only on the sharp focus area.','toggle');
  field(c,'neuralRuntimePath','Runtime file','Your nvngx_dlssnr.dll (tested version 310.8). Selected once: the app keeps a verified copy and uses it whenever this field is empty.','path',null,nr);
  const savedBox = document.createElement('div'); savedBox.id = 'savedRuntime'; savedBox.className = 'saved-runtime'; savedBox.hidden = true; $('setting-neuralRuntimePath').append(savedBox);
  // Hidden with Quad Views (updateControls): there DLSS 5 builds on DCS's own DLSS of the focus views.
  c = card('foveatedDlssControls','Foveated DLSS','Stereo only. Works with or without DLSS 5.');
  field(c,'foveatedDlss','Foveated Super Resolution','Performance: DLSS at full quality only where you look, lighter in the periphery. For stereo without Quad Views.','toggle',null,null,'',p => p.quadViews !== 'None');
  c = card('dlssProcessing','DLSS 5 image','Working scale and area set the GPU cost; intensity and style set the look.');
  field(c,'neuralWorkingScale','Working scale','Lower scale reduces GPU work and neural detail.','number',[.1,1,.01],nr,'ratio');
  field(c,'quadNeuralEdgeFade','DLSS 5 fades at the focus edge','The DLSS 5 look (tone, color, detail) fades out toward the edge of the focus area together with it, so the focus area does not stand out as a patch with a different look. No GPU cost.','toggle',null,p => nr(p) && p.quadViews === 'QuadViewsFoveated' && p.quadFocusAdapter && p.neuralFocusArea >= 100);
  field(c,'neuralFocusArea','DLSS 5 area','Part of each focus view DLSS 5 processes; the edge fades into normal DLSS. GPU time for both eyes, measured on an RTX 5090 at 1764×2480 per eye and full working scale.','select',[[100,'Whole focus area · 10.8 ms · default'],[80,'Central 80% · 7.9 ms'],[70,'Central 70% · 6.4 ms'],[50,'Central 50% · 4.8 ms']],p => nr(p) && p.quadViews === 'QuadViewsFoveated' && p.quadFocusAdapter);
  field(c,'neuralIntensity','Intensity','Blend strength of the neural image. Default 1.','number',[0,1,.01],nr,'ratio');
  field(c,'neuralStyle','Model style','The look of the DLSS 5 model.','select',[['Standard','Standard · default'],['Natural','Natural'],['Cinematic','Cinematic']],nr);
  field(c,'neuralBeforeUpscaling','Process at render resolution','On: before DLSS upscaling, faster but less detail. Off: at output resolution, more detail and more GPU.','toggle',null,nr);
  field(c,'neuralToggleKey','In-flight toggle key','Turns DLSS 5 on and off in flight to compare. Click, then press the keys. Default Ctrl+Shift+F12.','hotkey',null,nr);
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
  field(c,'neuralDepth','Depth convention','Follow the game or force a convention.','select',[['Game','Follow the game · default'],['Normal','Normal depth'],['Reversed','Reversed depth']],nr);
  field(c,'neuralMotionScaleX','Motion scale X','Horizontal motion vector multiplier.','number',[-4,4,.01],nr,'×');
  field(c,'neuralMotionScaleY','Motion scale Y','Vertical motion vector multiplier.','number',[-4,4,.01],nr,'×');
  // Frame generation page.
  c = card('framegenControls','Frame generation','OFXR adds generated frames between the frames DCS renders.');
  field(c,'frameGen','Frame generation','NVIDIA needs an RTX GPU; FidelityFX works on any GPU.','select',[['Off','Off'],['Nvidia','OFXR · NVIDIA Optical Flow'],['FidelityFx','OFXR · FidelityFX Optical Flow']]);
  field(c,'frameGenFactor','Generated frames','Auto renders at half the refresh rate and adds a second generated frame only while DCS cannot keep up. 3× always renders a third: more latency and more artifacts in sideways motion.','select',[[0,'Auto · 2×, 3× when DCS falls behind'],[2,'2× · one generated frame'],[3,'3× · two generated frames']],p => p.frameGen !== 'Off');
  field(c,'frameGenDeepPipeline','Smoothness buffer','On: OFXR keeps one more frame in flight, which is smoother when DCS frame times vary but adds about 11 ms of latency (one refresh at 90 Hz). In 3× it takes the extra slot only when needed. Off: lower latency, more stutter when DCS frame times vary. Applies to 2×, 3× and Auto.','toggle',null,p => p.frameGen !== 'Off');
  field(c,'smoothCursor','Smooth mouse cursor','Draws the cursor at its in-between position in generated frames, so it moves at the headset\'s rate. Where you click doesn\'t change.','toggle',null,p => p.frameGen !== 'Off');
  field(c,'nvidiaFlowScale','Optical flow resolution','Resolution used to estimate motion. Higher costs more GPU.','select',[[50,'50% · default'],[75,'75%'],[100,'100% · full']],flow);
  field(c,'flowPreset','Optical flow quality','More analysis costs more GPU.','select',[['Fast','Fast'],['Medium','Medium · default'],['Slow','Slow · most analysis']],flow);
  field(c,'bidirectionalFlow','Bidirectional flow','Estimate motion in both directions. Adds GPU work.','toggle',null,flow);
  more = disclosure(c,'In-headset diagnostics','Status panel and its key, VRAM counter, OFXR counter and log.');
  field(more,'diagnosticOverlayKey','Diagnostic panel key','Shows or hides FPS, DLSS 5 and frame generation status in the headset, for the current flight. Click, then press the keys. Default Alt+Shift+F12.','hotkey',null,p => p.frameGen !== 'Off');
  field(more,'diagnosticOverlayAtStart','Panel visible at start','Each flight starts with the diagnostic panel shown.','toggle',null,p => p.frameGen !== 'Off');
  field(more,'diagnosticVram','VRAM counter','Adds the video memory in use to the diagnostic panel.','toggle',null,p => p.frameGen !== 'Off');
  field(more,'showOverlay','OFXR FPS counter','OFXR\'s own counter, always on. The diagnostic panel already shows FPS when you want it.','toggle',null,p => p.frameGen !== 'Off');
  field(more,'diagnosticRecorder','OFXR log','Writes OFXR\'s log while flying, for troubleshooting.','toggle',null,p => p.frameGen !== 'Off');
  c = card('pacingControls','DCS frame limit','Written to DCS\'s options.lua; the original values are backed up when the profile is applied.');
  field(c,'fpsLimit','Frame limit','Match refresh: DCS renders exactly the frames the headset needs (half or a third with frame generation), so none are wasted.','select',[['Preserve','Keep the current DCS limit'],['MatchRefresh','Match refresh (recommended)'],['RuntimeHeadroom','Very high (300 FPS)'],['Custom','Custom limit']]);
  field(c,'refreshFromHeadset','Read the refresh rate from the headset','Pimax Play: the rate the headset runs at now, or in its last session. Sboys: SteamVR\'s refresh rate. Off: set it below.','toggle');
  field(c,'headsetRefreshHz','Headset refresh rate','Used when it is set by hand, or when the headset software has not reported one yet.','number',[50,240,.5],p => !p.refreshFromHeadset || !state?.headsetRefresh,'Hz');
  field(c,'renderedFpsCap','Custom limit','Frames DCS renders per second.','number',[30,300,.5],p => p.fpsLimit === 'Custom','FPS');
  field(c,'disableDcsVSync','Desktop VSync off','Stops VSync of the DCS window on the monitor from holding back the frame rate (graphics.sync = false). Headset sync is unaffected.','toggle');
  // CPU Boost page: started with Launch DCS, undone when DCS exits.
  c = card('boostControls','CPU Boost','Starts with Launch DCS in this app and is undone when DCS exits. The prefetch fix also works when DCS starts from Steam or a shortcut.');
  field(c,'cpuBoost','CPU Boost','Applies the options below to the next DCS launch.','toggle');
  field(c,'boostDcsPriority','DCS priority','Above normal suits most setups. High can starve audio, input and headset services when DCS uses every core.','select',[['Normal','Normal'],['AboveNormal','Above normal · recommended'],['High','High']],boost);
  field(c,'boostMoveVrRuntime','Move VR runtime and headset services','Keeps Pimax or SteamVR services off the cores DCS uses for its main and render threads.','toggle',null,p => boost(p) && !p.desktop);
  field(c,'boostMoveBackgroundApps','Move background apps','Moves the apps below to the slowest cores at below-normal priority while DCS runs.','toggle',null,boost);
  field(c,'boostBackgroundApps','Background apps','Process names, one per line, without .exe.','list',null,p => boost(p) && p.boostMoveBackgroundApps);
  field(c,'boostPrefetch','Prefetch fix','Stops DCS terrain threads from prefetching the same memory over and over. Measured: about 1.7 cores freed, 3–5% more FPS when CPU-bound, no change when GPU-bound. Tested in single player. Observe only counts the calls.','select',[['Off','Off'],['Observe','Observe · count only'],['Skip','Skip repeats · recommended']],boost);
  field(c,'boostElevated','Administrator rights','Asks once (UAC) when DCS starts, so services running under other accounts can be moved too. DCS still starts as your user. Without it those services are left as they are.','toggle',null,boost);
  // Flight helpers: each one on its own, with or without CPU Boost; started with Launch DCS, undone when DCS exits.
  c = card('boostControls','Flight helpers','Each works on its own, with or without CPU Boost. Started with Launch DCS and undone when DCS exits.');
  field(c,'freeVram','Free VRAM before flight','Closes the programs below when DCS starts, so their video memory is free for DCS. Each gets a normal close request first. No program or Windows setting is changed.','toggle');
  const vram = p => p.freeVram;
  field(c,'freeVramApps','Programs to close','Process names, one per line, without .exe; * and ? are wildcards. Voice chat (Discord) and recording (OBS) are left out on purpose.','list',null,vram);
  field(c,'freeVramReopen','Reopen after the flight','Starts the closed programs again, as your user, when DCS exits.','toggle',null,vram);
  field(c,'freeVramForce','End programs that do not close','After 5 seconds, ends a program that ignored the close request. Unsaved work in it is lost. Off: it keeps running.','toggle',null,vram);
  field(c,'pauseTobiiDesktop','Pause the desktop Tobii eye tracker','For a Tobii Eye Tracker 5 or 4C next to a Pimax Crystal: its software (Tobii Experience) also grabs the eye tracker of the headset, and Pimax eye tracking stops working. While DCS runs, the services of the desktop tracker are stopped; they start again when DCS exits. The eye tracking of the headset is never touched. Asks for administrator rights (UAC) when DCS starts.','toggle',null,p => !p.desktop);
  field(c,'smallDcsWindow','Small DCS window in VR','Sets the DCS window on the monitor to 1280×720, windowed (options.lua; your values return with Back to stock DCS). The headset image is unaffected: DCS and Windows just keep smaller desktop buffers, which saves some VRAM and GPU time.','toggle',null,p => !p.desktop);
  field(c,'lowerMonitor','Lower the monitor while flying','Switches the main monitor to the mode below when DCS starts and back when DCS exits. Never saved as a Windows setting: a restart also brings your mode back. HDR is left as it is.','toggle',null,p => !p.desktop);
  field(c,'flightDisplayMode','Monitor mode in flight','Modes your main monitor reports. 1920×1080 at 60 Hz suits most setups.','select',[[displayModeKey(profileDefaults),displayModeLabel(profileDefaults)]],p => p.lowerMonitor && !p.desktop,'',null,
    {prop:'flightDisplayMode',get:p => displayModeKey(p),set:(p,v) => { const [w,h,hz] = v.split(/[x@]/).map(Number); p.flightDisplayWidth = w; p.flightDisplayHeight = h; p.flightDisplayRefresh = hz; }});
  // Engine Optimizations page: the DcsQvCull module, installed in Saved Games with the profile.
  const engine = p => p.engineOptimizations;
  c = card('engineControls','Engine Optimizations','Measured together on a busy airfield in VR: about 50 to 60% more FPS depending on the scene, p95 frame time about 35% lower, and the GPU becomes the limit. No visual change.');
  field(c,'engineOptimizations','Engine Optimizations','Launch DCS (or Apply) installs a small module that DCS loads by itself from Saved Games\\DCS\\Scripts, so it stays on every time DCS starts, also from Steam or a desktop shortcut, until you turn it off or use Back to stock DCS. Nothing in the DCS install changes. Tested on DCS 2.9.30 in single player.','toggle');
  field(c,'engineShaderTimeCache','Streaming timer cache','DCS reads a high-resolution clock on every texture bind, only to timestamp texture-streaming use. The module serves it from a value refreshed every millisecond; streaming decisions are unchanged. Measured: 5 to 6% more FPS.','toggle',null,engine);
  field(c,'enginePartitionBoost','Culling partition boost','DCS splits the search for visible objects into 12 uneven tasks and waits for the slowest. The module splits the same work into 16; the same objects are found. Measured: 2% more FPS, p95 frame time 3% lower.','toggle',null,engine);
  field(c,'engineCostWeights','Culling partition by real cost','The split of the visible-object search into tasks follows what each object actually costs to check, measured as DCS runs, instead of a fixed estimate, so the tasks end together. Measured: 2.75% more FPS, p95 frame time 4% lower.','toggle',null,engine);
  field(c,'engineShadowInstancing','Shadow caster instancing (exact)','Identical objects casting shadows are drawn with one instanced draw per shadow cascade instead of one draw each. The module compiles instanced versions of the shadow shader DCS ships in the background (about 2.5 minutes after DCS starts); nothing in the DCS install is written. Shadow draws also skip the textures those shaders never read, and the grouping is planned off the render thread. Verified texel-identical; it turns itself off on any fault. Measured: 2.8% more FPS, and about 1.9% more from planning in the background.','toggle',null,engine);
  field(c,'engineModelAllocator','Model data allocator (parallel)','The threads that prepare aircraft and object models take memory from one shared allocator and wait for each other. The module gives each thread its own blocks. Measured: 6% more FPS, p95 frame time 9% lower.','toggle',null,engine);
  field(c,'engineFrameHeap','Frame memory per thread','Short-lived per-frame memory comes from one shared heap the model threads queue for. Each thread gets its own blocks; near the end of the heap the original is used. Measured: 1.5% more FPS, culling 9% faster.','toggle',null,engine);
  field(c,'engineTaskQueueClock','Task queue clock cache (camera motion)','At the start of each frame DCS drains its task queue and reads the clock for every task. Only that unlimited drain uses the cached clock. Helps while the camera moves fast: render-thread drain 2.9 to 1.6 ms per frame in a fast flight; nothing changes when static. Needs the streaming timer cache.','toggle',null,p => engine(p) && p.engineShaderTimeCache);
  field(c,'engineTextureDedupe','Texture streaming dedupe','DCS often asks the streamer for the same texture at the same size several times within a millisecond. The repeats are skipped. Measured: 2% more FPS.','toggle',null,engine);
  field(c,'engineGBufferRecorder','Multi-threaded G-buffer','DCS draws the scene\u2019s models into its G-buffer on the render thread, one at a time. The module records those draws on worker threads in DCS\u2019s exact order and DCS replays them. Verified pixel-identical; measured 22 to 24% more FPS on its own. With everything on, the GPU usually becomes the limit.','toggle',null,engine);
  field(c,'engineShadowRecorder','Multi-threaded shadows','DCS draws the shadow cascades on its render thread, one object at a time. The module records those draws on 8 worker threads and DCS replays them, so the render thread waits less. Verified texel-identical; measured 3 to 5% more FPS and 4 to 7% shorter p95 frames. Total CPU use rises a little because the work moves to other cores.','toggle',null,engine);
  field(c,'engineStateFilter','Redundant state filter','DCS sets graphics state (shaders, buffers, textures) that is already set, hundreds of thousands of times a frame. The repeat is skipped where DCS makes the call. Verified identical; measured about 1.2% less CPU and 0.5 to 1% more FPS. Turns itself off after a DCS update.','toggle',null,engine);
  field(c,'engineEffectBufferSkip','Effect constant-buffer skip','The effect system sets the same constant buffer again although it is already set. The repeat is skipped. Measured together with the counter below: 3.6% more FPS.','toggle',null,engine);
  field(c,'enginePlainCounter','Statistics counter without lock','A triangle counter used only for statistics is updated without a locked instruction, which made all model threads wait on one cache line.','toggle',null,engine);
  field(c,'engineToggleKey','In-flight switch','Turns all the optimizations above off and back on in flight, to compare: one beep off, two beeps on. Click, then press the keys. Default Alt+Shift+F11.','hotkey',null,engine);
  field(c,'engineBeeps','Beeps','Sound feedback for the in-flight switch and the test suite.','toggle',null,engine);
  field(c,'engineDevMode','Developer mode','Off for normal flying and for testing a release. On: for working on the module, DCS hot-reloads the payload below whenever it changes and takes every engine setting from the settings file below instead of this page.','toggle',null,engine);
  field(c,'engineDevPayloadPath','Developer payload','A DcsQvCullPayload.dll built elsewhere. While it is missing, the installed copy runs.','path',null,p => engine(p) && p.engineDevMode);
  field(c,'engineDevIniPath','Developer settings file','A DcsQvCull.ini that replaces the one this profile installs. A run_suite.flag next to it starts the test suite too.','path',null,p => engine(p) && p.engineDevMode);
  more = disclosure(c,'Advanced','Keep the defaults unless you are troubleshooting.');
  field(more,'engineTimerRefreshUs','Streaming timer refresh','How often the cached time is refreshed. 1000 µs (1 ms) is far below one frame.','number',[250,2000,50],p => engine(p) && p.engineShaderTimeCache,'µs');
  field(more,'engineDiagnosticHooks','Measurement hooks always on','Normally installed only while the test suite runs. For troubleshooting; costs some CPU.','toggle',null,engine);
  // DCS & headset page.
  c = card('gameControls','DCS','Detected automatically; change only if DCS is somewhere else.');
  field(c,'dcs','DCS executable','DCS.exe in the bin folder of your installation.','path');
  field(c,'options','DCS settings file','Saved Games\\DCS\\Config\\options.lua.','path');
  field(c,'keepDcsLauncher','Show the DCS launcher','Off: Launch DCS goes straight into the game (the launcher setting is switched off with the profile and restored with it). On: the DCS launcher opens first; press Play there.','toggle');
  // Pupil shift (native/pupil_shift): an OpenXR layer first in the chain; it needs the gaze of eye-tracked Quad Views.
  const pupil = p => p.pupilShift;
  c = card('pupilControls','<span class="tag">EXPERIMENTAL</span> Pupil shift','When your eyes turn, each pupil moves a few millimetres. DCS normally renders both eyes from fixed points, so near objects shift slightly against far ones as you look around. Not yet confirmed in flight; turn it off if anything looks wrong.');
  field(c,'pupilShift','Pupil shift','Renders each eye from where its pupil is for your gaze, so the cockpit stays put when only your eyes move. About 0.2 to 0.4 degrees at 25 degrees of gaze. No GPU cost.','toggle');
  field(c,'pupilShiftEyeRadiusMm','Eye rotation radius','From the pupil to the point the eye turns around. About 10 to 11 mm for most people; more makes the shift larger.','number',[5,15,.5],pupil,'mm');
  field(c,'pupilShiftVirtualImageM','Lens image distance','How far away the lens shows the image. Pimax does not publish it; 1 to 2 m is typical. It changes only distant objects, by less than 0.1 degree.','number',[.5,5,.1],pupil,'m');
  field(c,'pupilShiftToggleKey','In-flight switch','Turns the correction off and back on in flight, to compare: a low beep off, a high beep on. Click, then press the keys. Default Ctrl+Shift+F10.','hotkey',null,pupil);
  c = card('headsetControls','Headset runtime','Uses the headset software chosen under How you fly in the right panel (Pimax Play or SteamVR).');
  more = disclosure(c,'Runtime override','Only for a runtime that is not registered.');
  field(more,'runtimeManifestPath','OpenXR runtime file','An OpenXR runtime JSON to use instead of the route\'s registered runtime. Leave empty normally. Not used with Optimizations only.','path',null,p => !p.desktop);
}
function buildFeatureList() {
  // The optimizations every pilot can use, then the VR features under their own label (hidden with Optimizations only).
  const groupLabel = key => key === 'engine' ? '<div class="feature-group" id="featureGroup-all">Every pilot</div>' : key === 'quad' ? '<div class="feature-group" id="featureGroup-vr">VR</div>' : '';
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
  $('pageTitle').textContent = entry[1]; $('pageDescription').textContent = id === 'dlss' && profile && profile.quadViews !== 'None' ? 'DLSS 5 neural rendering on the Quad Views focus area.' : entry[2]; $('scrollArea').scrollTop = 0;
  if (id === 'foveation' && profile) rereadPimax(false);
  if (id === 'boost' && profile) refreshBoostPlan(0);
  if (id === 'engine' && profile) refreshEngine();
  if (id === 'pupil' && profile) { renderPupilNeeds(); refreshPupil(); }
  if (id === 'support' && profile) refreshDlssFocus();
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
  if (!pimax.found) { box.innerHTML = `<div class="pimax-head"><div><b>Pimax Play settings not found</b><small>${escape(pimax.path)}</small></div>${reread}</div><p class="fine">${mode === 'bundled' ? "This profile's own focus values are used until Pimax Play's settings can be read. Select This profile to edit them." : 'Pimax applies its own values.'}</p>`; return; }
  // Exactly what Pimax Play's Quad View page shows for the selected mode: its labels, its order, its rounding.
  const open = Boolean(box.querySelector('details.pimax-details')?.open);
  const controls = `<dl class="pimax-controls" aria-label="Pimax Play Quad View settings">${pimax.controls.map(x => `<dt>${escape(x.label)}</dt><dd>${escape(x.value)}</dd>`).join('')}</dl>`;
  const warnings = [pimax.mismatch, mode === 'bundled' ? pimax.capNote : null].filter(Boolean).map(n => `<p class="pimax-note warn">${escape(n)}</p>`).join('');
  let how = `<p class="fine">Pimax native Quad Views applies these values itself. Nothing is written to Pimax.</p>`;
  if (mode === 'bundled') {
    const c = pimax.converted, e = effectiveFovea(profile), r = pimax.eyeResolution, share = v => num(v*100)+'%';
    const pixels = r ? `<dt>Focus per eye at ${r.width} × ${r.height}</dt><dd>${r.focusWidth} × ${r.focusHeight} px</dd>` + (c.clamped ? `<dt>Pimax's own focus per eye</dt><dd>${r.pimaxFocusWidth} × ${r.pimaxFocusHeight} px</dd>` : '') : '';
    const notes = pimax.notes.filter(n => n !== pimax.mismatch && n !== pimax.capNote).map(n => `<p class="pimax-note">${escape(n)}</p>`).join('');
    how = `<details class="pimax-details"${open ? ' open' : ''}><summary>How bundled Quad Views reproduces this</summary><p class="fine">${escape(pimax.sameLabelNote)}</p>`
      + `<dl class="pimax-values"><dt>Focus share of the view</dt><dd>${share(c.width)} × ${share(c.height)}</dd><dt>Focus pixel density</dt><dd>${num(c.focusScale)}×</dd><dt>Peripheral factor</dt><dd>${num(c.peripheralScale)}</dd><dt>Edge blending</dt><dd>${num(e.blend)}</dd>${pixels}</dl>`
      + (r ? `<p class="fine">${r.width} × ${r.height} is the per-eye resolution the headset runtime recommended in DCS's last session; sizes are rounded to an even pixel count as Pimax's runtime does.</p>` : '')
      + `${notes}<p class="fine">${escape(pimax.explanation)}</p></details>`;
  }
  box.innerHTML = `<div class="pimax-head"><div><b>Pimax Play · ${escape(pimax.mode)} mode</b><small>${escape(pimax.path)} · saved ${new Date(pimax.modified).toLocaleString('en-GB')}</small></div>${reread}</div>${controls}${warnings}${how}`;
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
  const kept = retained ? `<p class="fine">The forgotten copy stays at ${escape(retained)} because the applied profile was installed from it. It is deleted when that profile is restored.</p>` : '';
  if (!s) { box.innerHTML = kept; return; }
  box.innerHTML = `<div class="saved-runtime-head"><div><b>Saved copy · version ${escape(s.version || 'unknown')} · from ${escape(s.originalPath)}</b><small>Kept at ${escape(s.path)} · NVIDIA signature verified · saved ${new Date(s.savedAt).toLocaleString('en-GB')}${s.originalExists ? '' : ' · the original file is gone, the copy is still used'}</small></div><div class="saved-runtime-actions"><button type="button" class="button secondary" id="changeRuntime"${busy ? ' disabled' : ''}>Change…</button><button type="button" class="button quiet" id="forgetRuntime"${busy ? ' disabled' : ''}>Forget</button></div></div>`
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
  if (!s) { box.innerHTML = '<p class="fine">Select the DCS settings file (options.lua) on the DCS &amp; headset page first.</p>'; return; }
  const report = s.lastReport ? `<dt>Last test report</dt><dd>${escape(new Date(s.lastReportAt).toLocaleString('en-GB'))}</dd>` : '';
  const pending = profile?.engineOptimizations !== (s.state !== 'not-installed') ? `<p class="fine">${profile.engineOptimizations ? 'Launch DCS installs it with this profile.' : 'Launch DCS removes it with this profile.'}</p>` : '';
  const devNote = profile?.engineOptimizations && profile.engineDevMode ? '<p class="pimax-note warn">Developer mode is on in this profile: DCS loads the payload and settings from the developer paths, not the ones this release installs. Turn it off on the left for normal flying.</p>' : '';
  box.innerHTML = devNote + `<div class="engine-state ${escape(s.state)}"><b>${escape(ENGINE_STATE[s.state] || s.state)}</b><span>${escape(s.summary)}</span></div>${pending}`
    + s.warnings.map(w => `<p class="pimax-note warn">${escape(w)}</p>`).join('')
    + (s.suiteRunning ? '<p class="pimax-note">Test suite running in DCS. Keep the view still (in VR, keep the headset on); it takes about 7 minutes.</p>' : '')
    + `<dl class="pimax-values">${report}</dl>`;
  $('engineSuite').disabled = busy || s.state !== 'loaded' || s.suiteRunning;
  $('engineReport').disabled = busy || !s.lastReport;
  $('engineLog').disabled = busy || !s.logPath;
}
async function engineAction(action, data) {
  if (busy) return;
  try { const result = await request(action, data); if (result?.message) $('status').textContent = result.message; refreshEngine(); }
  catch (error) { showError(error); }
}
// ---- Pupil shift -------------------------------------------------------------------------------------------------
/** The layer is installed only for a VR flight with eye-tracked Quad Views (VrProfile.UsesPupilShift). */
const usesPupilShift = p => Boolean(p.pupilShift && !p.desktop && p.quadViews !== 'None' && p.gaze === 'EyeTracked');
/** What the profile still needs for a gaze, with a link to the setting that gives it. */
function renderPupilNeeds() {
  const ipd = $('pupilIpd');
  if (ipd) { const mm = state?.dcsCustomIpd; ipd.hidden = !mm || !profile?.pupilShift; ipd.textContent = mm ? `DCS's custom IPD (${mm} mm) is applied by Pupil shift: the same ${mm} mm, set by the layer, because DCS would otherwise reset each eye's sideways position and undo the pupil's sideways move. Launch DCS turns DCS's own option off while this profile is applied; Back to stock DCS turns it on again.` : ''; }
  const box = $('pupilNeeds'); if (!box || !profile) return;
  const need = profile.quadViews === 'None' ? ['Pupil shift needs Quad Views with eye-tracked focus: it reads your gaze from the focus area. Quad Views is off.','quadViews','Turn on Quad Views']
    : profile.gaze !== 'EyeTracked' ? ['Pupil shift needs eye-tracked focus: it reads your gaze from the focus area, and Focus movement is Fixed.','gaze','Set Focus movement'] : null;
  box.hidden = !need;
  box.innerHTML = need ? `<p>${need[0]}${profile.pupilShift ? ' Until then it is not installed.' : ''}</p><button class="button secondary" id="pupilFix">${need[2]}</button>` : '';
  if (need) $('pupilFix').addEventListener('click',() => revealSetting(fields.find(f => f.path === need[1])));
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
const PUPIL_STATE = {'not-installed':['Not installed',''], waiting:['Installed',''], working:['Working','loaded'], 'no-gaze':['No gaze','failed'], off:['Switched off',''], 'not-restored':['Check the log','failed']};
const degrees = r => r ? `${r.min > 0 ? '+' : ''}${r.min.toFixed(0)}° to ${r.max > 0 ? '+' : ''}${r.max.toFixed(0)}°` : '—';
function renderPupil() {
  const s = pupilStatus, box = $('pupilStatus'); if (!box) return;
  if (!s) { box.innerHTML = '<p class="fine">Reading the layer\'s log…</p>'; $('pupilLog').disabled = true; return; }
  const installed = s.state !== 'not-installed';
  const pending = profile && usesPupilShift(profile) !== installed ? `<p class="fine">${usesPupilShift(profile) ? 'Launch DCS installs it with this profile.' : 'Launch DCS removes it with this profile.'}</p>` : '';
  const [label, tone] = PUPIL_STATE[s.state] || [s.state,''];
  const rows = s.seconds ? [
    ['Gaze range, left–right', degrees(s.sessionGazeX)], ['Gaze range, up–down', degrees(s.sessionGazeY)],
    ['Largest pupil shift', s.sessionMaxShiftMm ? s.sessionMaxShiftMm.toFixed(1)+' mm' : '—'],
    ['Seconds with a gaze', `${s.secondsWithGaze} of ${s.seconds}`],
    ['Runtime poses given back', s.restoredViews == null ? '—' : s.restoredViews > 0 ? 'Yes: the headset places the image as without the layer' : 'No'],
    ['In-flight switch', s.hotkey ? keyLabel(parseKey(s.hotkey)) : '—'],
    ['Headset views turned outwards (cant)', s.last?.cantDeg != null ? s.last.cantDeg.toFixed(1)+'°, straight ahead taken between them' : '—'],
    ['Eye distance: headset / DCS renders', s.last?.runtimeIpdMm != null ? `${s.last.runtimeIpdMm.toFixed(1)} / ${s.last.renderedIpdMm.toFixed(1)} mm` : '—']
  ] : [];
  box.innerHTML = `<div class="engine-state ${tone}"><b>${escape(label)}</b><span>${escape(s.summary)}</span></div>${pending}`
    + s.warnings.map(w => `<p class="pimax-note warn">${escape(w)}</p>`).join('')
    + (rows.length ? `<dl class="pimax-values">${rows.map(([k,v]) => `<dt>${escape(k)}</dt><dd>${escape(v)}</dd>`).join('')}</dl>` : '');
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
    const rows = [['DLSS-NR runtime',r.version],['Focus views identified',r.accepted ? String(r.accepted) : null],['Rejections',r.rejected ? String(r.rejected) : null],
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
  const off = (on,text) => on ? '' : `<div class="inline-note">${text}</div>`;
  const rows = [['DCS main and render threads',b.renderCores],['VR runtime and headset services',b.vrRuntimeCores],['Background apps',b.backgroundCores]];
  const processes = b.processes.length ? `<table class="boost-table"><thead><tr><th>Process</th><th>Change</th><th>Running</th><th>Admin</th></tr></thead><tbody>${b.processes.map(p => `<tr><td><b>${escape(p.name)}</b><small>${escape(p.category)}</small></td><td>${escape(p.action)}</td><td>${p.running ? 'Yes' : 'No'}</td><td>${p.needsAdministrator ? (profile.boostElevated ? 'Yes · asked at launch' : 'Needed · left as is') : '—'}</td></tr>`).join('')}</tbody></table>` : '<p class="fine">No processes would change.</p>';
  const cpu = `<h3 class="boost-section">CPU Boost</h3>` + off(profile.cpuBoost,'CPU Boost is off. This is what it would do when turned on.') + `<dl class="boost-cores"><dt>Core ranking</dt><dd>${escape(b.sourceDescription)}</dd>${rows.map(([k,v]) => `<dt>${k}</dt><dd>${escape(cores(v))}</dd>`).join('')}<dt>DCS log</dt><dd>${escape(dcsLog)}</dd></dl>` + processes + (b.notes.length ? `<ul class="boost-notes">${b.notes.map(n => `<li>${escape(n)}</li>`).join('')}</ul>` : '');
  // Free VRAM before flight: each listed program, running processes and their video memory now (approximate).
  const apps = b.freeVram || [];
  const vram = `<h3 class="boost-section" id="vramPlanTitle">Free VRAM before flight</h3>` + off(profile.freeVram,'Off. This is what it would close now.') + (apps.length
    ? `<table class="boost-table vram-table"><thead><tr><th>Program</th><th>Running</th><th>VRAM (approx.)</th></tr></thead><tbody>${apps.map(a => `<tr><td><b>${escape(a.name)}</b>${a.skipped ? '<small>Protected processes skipped</small>' : a.needsAdministrator ? '<small>Needs admin rights to close</small>' : ''}</td><td>${a.processes ? a.processes+(a.processes === 1 ? ' process' : ' processes') : 'No'}</td><td>${a.processes ? megabytes(a.dedicatedBytes) : '—'}</td></tr>`).join('')}</tbody><tfoot><tr><td><b>Total to free</b></td><td></td><td id="vramTotal"><b>${megabytes(b.freeVramBytes)}</b></td></tr></tfoot></table>`
      + '<p class="fine">Per-process GPU counters; memory shared between programs can be counted twice, so the total is approximate.</p>'
    : '<p class="fine">The list is empty: nothing would be closed.</p>');
  const m = b.monitor, mode = x => `${x.width}×${x.height} at ${x.refresh} Hz`;
  const monitor = `<h3 class="boost-section">Monitor while flying</h3>` + off(profile.lowerMonitor && !profile.desktop,profile.desktop ? 'Not with Optimizations only.' : 'Off. The monitor keeps its mode.') + (m
    ? `<dl class="boost-cores" id="monitorPlan"><dt>Display</dt><dd>${escape(m.name)}</dd><dt>Mode</dt><dd>${escape(mode(m.current))} → ${escape(mode(m.flight))}</dd></dl><p class="fine">${escape(m.note)}</p>`
    : '<p class="fine">No display could be read.</p>');
  const small = `<h3 class="boost-section">Small DCS window in VR</h3>` + off(profile.smallDcsWindow && !profile.desktop,profile.desktop ? 'Not with Optimizations only.' : 'Off. The DCS window keeps your size.') + `<p class="fine">options.lua: ${escape(b.smallWindow || '')}. Back to stock DCS puts your values back.</p>`;
  const tobii = `<h3 class="boost-section">Desktop Tobii eye tracker</h3>` + off(profile.pauseTobiiDesktop && !profile.desktop,profile.desktop ? 'Not with Optimizations only.' : 'Off. A desktop Tobii tracker keeps running.') + ((b.tobiiServices || []).length
    ? `<p class="fine">Paused while DCS runs, started again when it exits: ${(b.tobiiServices || []).map(s => '<b>'+escape(s)+'</b>').join(', ')}. The eye tracking services of the headset are not touched.</p>`
    : '<p class="fine">No desktop Tobii eye tracker is installed on this PC: nothing would be paused.</p>');
  // Same order as the switches on the left.
  $('boostPlan').innerHTML = cpu + vram + tobii + small + monitor;
  fillDisplayModes(m);
}
function updateControls() {
  if (!profile) return;
  document.querySelectorAll('input[type=range]').forEach(paintRange);
  fields.forEach(f => { const inactive = Boolean(f.condition && !f.condition(profile)); f.element.classList.toggle('inactive',inactive); f.input.disabled = busy || inactive; if (f.range) f.range.disabled = busy || inactive; f.element.hidden = Boolean(f.hide && f.hide(profile)); });
  // Pimax native exists only on the Pimax route and cannot host the focus adapter DLSS needs.
  const native = $('input-quadViews').querySelector('[value=PimaxNative]'); native.disabled = !nativeAllowed(profile);
  native.textContent = 'Pimax native Quad Views' + (profile.runtime === 'SboysSteamVr' ? ' · Pimax route only' : native.disabled ? ' · not with DLSS 5' : '');
  // Foveated Super Resolution is a stereo option: its card is gone while Quad Views is on.
  $('foveatedDlssControls').hidden = profile.quadViews !== 'None';
  // Stereo Cheeky clamps these ratios to 0.2 or more: say which value is written when the field holds less.
  fields.filter(f => ['foveaWidth','foveaHeight','peripheralScale'].includes(f.path)).forEach(f => {
    const help = $('help-'+f.path), clamped = stereoCheeky(profile) && Number(profile[f.path]) < CHEEKY_MIN;
    help.textContent = f.description + (clamped ? ` Without Quad Views, Cheeky uses at least ${CHEEKY_MIN}, so ${CHEEKY_MIN} is written.` : '');
    f.element.classList.toggle('clamped', clamped);
  });
  // Bundled Quad Views renders at most 90% of the view per axis: Horizontal or Vertical FOV 10% in Pimax Play's Quick units.
  fields.filter(f => ['pimaxHorizontalFov','pimaxVerticalFov'].includes(f.path)).forEach(f => {
    const value = f.map.get(profile), capped = quad(profile) && !pimaxSource(profile) && PIMAX.quickFromShare(profile[f.path === 'pimaxHorizontalFov' ? 'foveaWidth' : 'foveaHeight']) < 10 - 1e-9;
    $('help-'+f.path).textContent = f.description + (capped ? ` Bundled Quad Views renders a focus of at most 90% of the view, so ${value}% renders as 10%.` : '');
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
    ['framegen','FRAME GENERATION',on.framegen,profile.frameGen === 'Off' ? 'Off' : profile.frameGen === 'Nvidia' ? 'OFXR · NVIDIA' : 'OFXR · FidelityFX',featureSummary('framegen',profile)]
  ];
  // Two groups: the optimizations every pilot can use, then the VR features (not with Optimizations only).
  const names = {engine:'Engine Optimizations',boost:'CPU Boost',foveation:'Quad Views',dlss:'DLSS 5',framegen:'Frame generation'};
  const tile = ([target,label,active,title,detail]) => { if (!active) { title = names[target] || title; detail = String(detail).replace(/^Off · /,''); } return `<button type="button" class="stage ${active ? 'on' : 'off'}" data-page="${target}" aria-label="${escape(title)}: ${active ? 'on' : 'off'}. Open settings"><span>${label}<em>${active ? 'ON' : 'OFF'}</em></span><b>${escape(title)}</b><small>${escape(detail)}</small></button>`; };
  const group = (label, list) => `<div class="stage-group"><div class="stage-group-label">${label}</div><div class="stage-row${list.length === 2 ? ' two' : ''}">${list.map(tile).join('')}</div></div>`;
  const everyone = tiles.filter(([target]) => !VR_PAGES.includes(target)), vr = tiles.filter(([target]) => VR_PAGES.includes(target));
  $('pipeline').innerHTML = group('For every pilot · monitor or any headset', everyone) + (profile.desktop ? '' : group('VR · ' + (profile.runtime === 'SboysSteamVr' ? 'Sboys' : 'Pimax'), vr));
  // What DLSS 5 still needs, on its own page; everything else to fix is listed once, in Checks.
  const notes = featureNotes(profile);
  $('dlssMissing').innerHTML = notes.dlss.filter(n => n[0] === 'needs').map(() => `<div class="needs"><span>DLSS 5 needs your nvngx_dlssnr.dll runtime file.</span></div>`).join('');
  // Inside a job that forbids breakaway the DCS launcher cannot start the game (LaunchSafety.CurrentJob).
  const launcher = fields.find(f => f.path === 'keepDcsLauncher'), blocked = Boolean(state?.launcherBlocked);
  launcher.element.querySelector('.switch-row p').textContent = launcher.description + (blocked ? ' Not available now: this copy of DCS Control was started by another program inside a Windows job, where the launcher cannot start the game (error 5). Open DCS Control from the Start menu or Explorer to use it.'
    : state?.launcherUnknown ? ' Unknown here: this copy runs inside a Windows job whose limits could not be read. If DCS does not open after the launcher, open DCS Control from the Start menu or Explorer.' : '');
  launcher.element.classList.toggle('clamped', blocked);
  if (blocked && !profile.keepDcsLauncher) { launcher.input.disabled = true; launcher.element.classList.add('inactive'); }
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
  VR_PAGES.forEach(id => $('nav-'+id).hidden = Boolean(profile.desktop));
  $('panelDevice').textContent = profile.desktop ? 'Any headset, or the monitor' : profile.runtime === 'SboysSteamVr' ? 'Pimax headset · SteamVR (Sboys)' : 'Pimax headset · Pimax Play';
  if (profile.desktop && VR_PAGES.includes(page)) navigate('overview');
  // The Sboys driver is only needed on the Sboys route.
  $('sboysCard').hidden = profile.runtime !== 'SboysSteamVr' || Boolean(profile.desktop);
  $('headsetControls').hidden = Boolean(profile.desktop);
  renderCadence();
  updateWorkflow(); updatePanelScroll();
}
/** Shadow above the workflow footer while part of the right panel is scrolled out of view below. */
function updatePanelScroll() {
  const top = document.querySelector('.panel-top');
  document.querySelector('.workflow').classList.toggle('more-above', top.scrollHeight - top.clientHeight - top.scrollTop > 1);
}
function renderCadence() {
  const fg = profile.frameGen !== 'Off', multiplier = fg ? (profile.frameGenFactor === 3 ? 3 : 2) : 1, hz = refreshHz(profile), target = hz / multiplier;
  const detected = Number(state?.inventory?.dcsSettings?.['graphics.maxFPS']);
  const cap = profile.fpsLimit === 'Preserve' ? (Number.isFinite(detected) && detected > 0 ? detected : null) : profile.fpsLimit === 'RuntimeHeadroom' ? 300 : profile.fpsLimit === 'MatchRefresh' ? Math.max(30, target) : profile.renderedFpsCap;
  const n = value => Number(value.toFixed(3)).toLocaleString('en-US');
  const metrics = [
    ['DCS NEEDS TO RENDER',n(target)+' FPS',fg ? (profile.frameGenFactor === 0 ? 'Half the refresh rate; a third while DCS falls behind.' : multiplier === 3 ? 'A third of the refresh rate.' : 'Half the refresh rate.') : 'The full refresh rate; frame generation is off.'],
    ['DCS FRAME LIMIT',cap === null ? 'Not set' : n(cap)+' FPS',profile.fpsLimit === 'Preserve' ? (cap === null ? 'No limit found in options.lua; left as it is.' : 'Found in options.lua; left as it is.') : 'Written to options.lua when the profile is applied.'],
    ['HEADSET',n(hz)+' Hz' + (profile.refreshFromHeadset && state?.headsetRefresh ? ' · '+state.headsetRefresh.source : ''),fg ? `Up to ${n(cap === null ? hz : Math.min(hz,cap*multiplier))} FPS with generated frames.` : 'Set this refresh rate in Pimax Play or SteamVR.']
  ];
  $('cadenceMetrics').innerHTML = metrics.map(b => `<div class="budget"><span>${b[0]}</span><strong>${b[1]}</strong><p>${b[2]}</p></div>`).join('');
  const warnings = [];
  if (cap !== null && cap + .01 < target) warnings.push(`The frame limit (${n(cap)} FPS) is below what DCS needs to render (${n(target)} FPS). Raise the limit or choose a lower headset refresh rate.`);
  if (state?.inventory?.autoexecPath && (state.inventory.autoexecMaxFps !== null || state.inventory.autoexecPacingUnknown)) warnings.push('autoexec.cfg sets its own limit'+(state.inventory.autoexecMaxFps ? ' (max_fps = '+state.inventory.autoexecMaxFps+')' : '')+'. It is left as it is; check it does not hold DCS back.');
  $('cadenceWarnings').innerHTML = warnings.map(w => `<div class="issue Warning">${escape(w)}</div>`).join('');
}
// "Applied" means the draft has the applied profile.json's content; id and name only follow the features.
const canonical = value => Array.isArray(value) ? value.map(canonical) : value && typeof value === 'object'
  ? Object.fromEntries(Object.keys(value).filter(k => k !== 'id' && k !== 'name').sort().map(k => [k, canonical(value[k])])) : value;
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
  $('reviewPlan').disabled = busy || hasInvalid || !profile;
  $('openRecovery').hidden = !state?.originals?.count; $('openRecovery').disabled = busy || mode === 'running';
  $('saveProfile').disabled = busy || hasInvalid;
  const error = $('statusDot').classList.contains('error');
  $('statusTitle').textContent = busy ? 'Working' : hasInvalid ? 'Check numeric values' : error ? 'Needs attention'
    : {running:'DCS is running', ready:'Ready to fly', changes:'Changes apply at launch'}[mode];
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
  try { const {dcsRunning} = await request('dcsStatus'); if (state && dcsRunning !== Boolean(state.dcsRunning)) { state.dcsRunning = dcsRunning; renderOriginals(); updateWorkflow(); renderDashboard(); loadFlight(dcsRunning ? 0 : 3000); } } catch { }
}, 4000);
// ---- Checks: one list, grouped Must fix / Check yourself / OK, each line with its way to the fix ---------------------
// Titles and links for check ids (ProfileValidation codes, Readiness ids). [title, {reveal|page|guide, label, browse}]
const CHECK_INFO = {
  'neural-runtime':['DLSS 5 runtime file',{reveal:'neuralRuntimePath',label:'Select file',browse:true}],
  'dcs-not-found':['DCS executable',{reveal:'dcs',label:'Select',browse:true}], 'options-not-found':['DCS settings file',{reveal:'options',label:'Select',browse:true}],
  'game':['DCS and Saved Games',{reveal:'dcs',label:'Open'}], 'dcs-launcher':['DCS launcher',{reveal:'keepDcsLauncher',label:'Open'}],
  'dcs-running':['DCS is running',null], 'closed':['DCS is closed',null],
  'neural-hotkey':['DLSS 5 toggle key',{reveal:'neuralToggleKey',label:'Change'}], 'neural-hotkey-dcs':['DLSS 5 toggle key',{reveal:'neuralToggleKey',label:'Change'}],
  'diagnostic-hotkey':['Diagnostic panel key',{reveal:'diagnosticOverlayKey',label:'Change'}], 'diagnostic-hotkey-dcs':['Diagnostic panel key',{reveal:'diagnosticOverlayKey',label:'Change'}],
  'hotkey-conflict':['In-flight keys',{reveal:'diagnosticOverlayKey',label:'Change'}],
  'pimax-fovea':['Focus area from Pimax Play',{page:'foveation',label:'Open'}], 'quad-coverage-cap':['Focus area size',{page:'foveation',label:'Open'}],
  'quad-cheeky':['Quad Views and DLSS',{page:'foveation',label:'Open'}], 'quad-runtime':['Quad Views provider',{reveal:'quadViews',label:'Open'}], 'quad-adapter-provider':['Quad Views provider',{reveal:'quadViewsLayerDirectory',label:'Open'}],
  'quad-focus':['Focus adapter',null], 'pimax-native-replaced':['Quad Views provider',null], 'fovea-source':['Focus area source',{reveal:'foveaSource',label:'Open'}],
  'boost-launcher':['CPU Boost app lists',{page:'boost',label:'Open'}], 'boost-app-name':['CPU Boost app lists',{page:'boost',label:'Open'}], 'boost-app-wildcard':['CPU Boost app lists',{page:'boost',label:'Open'}],
  'boost-choice':['CPU Boost',{page:'boost',label:'Open'}], 'monitor-mode':['Monitor mode in flight',{reveal:'flightDisplayMode',label:'Open'}], 'boost-high-priority':['DCS priority',{reveal:'boostDcsPriority',label:'Open'}],
  'framegen-factor':['Generated frames',{reveal:'frameGenFactor',label:'Open'}], 'flow-preset':['Optical flow',{reveal:'flowPreset',label:'Open'}], 'flow-scale':['Optical flow',{reveal:'nvidiaFlowScale',label:'Open'}],
  'fps-mode':['DCS frame limit',{reveal:'fpsLimit',label:'Open'}], 'fps-under-target':['DCS frame limit',{reveal:'fpsLimit',label:'Open'}],
  'refresh-rate':['Refresh rate',{reveal:'refreshFromHeadset',label:'Open'}], 'external-limiters':['Other FPS limits',null], 'runtime-reprojection':['Motion smoothing',null],
  'route-mismatch':['Headset route',{page:'setup',label:'Open'}], 'steamvr-not-found':['SteamVR',{guide:'steamvr',label:'Guide'}],
  'gaze-bridge-missing':['Eye tracking on Sboys',{guide:'gaze',label:'Guide'}], 'runtime':['OpenXR runtime',{page:'setup',label:'Open'}],
  'active-backup':['Applied profile',{page:'recovery',label:'Stock DCS'}], 'deployment':['Files and components',{page:'diagnostics',label:'Review',review:true}],
  'write-access':['Write access to game folders',{action:'grantWriteAccess',label:'Grant access'}], 'write-blocked':['Write access to game folders',null],
  'dlss-gpu':['DLSS 5 graphics card',{page:'dlss',label:'Open'}], 'framegen-gpu':['Frame generation graphics card',{reveal:'frameGen',label:'Open'}],
  'pupil-gaze':['Pupil shift',{page:'pupil',label:'Open'}], 'pupil-radius':['Pupil shift',{reveal:'pupilShiftEyeRadiusMm',label:'Open'}], 'pupil-image':['Pupil shift',{reveal:'pupilShiftVirtualImageM',label:'Open'}],
  'pupil-hotkey':['Pupil shift switch',{reveal:'pupilShiftToggleKey',label:'Open'}], 'pupil-hotkey-dcs':['Pupil shift switch',{reveal:'pupilShiftToggleKey',label:'Open'}],
  'neural-area':['DLSS 5 area',{reveal:'neuralFocusArea',label:'Open'}], 'neural-depth':['Depth convention',{reveal:'neuralDepth',label:'Open'}]
};
const GROUPS = [['fix','Must fix'],['check','Check yourself'],['ok','OK']];
/** Profile issues (every edit) and, after Check this PC, the readiness checks; one entry per id. */
function checkItems() {
  const items = [], seen = new Set();
  const add = (id,group,title,text,guide) => { const key = id+'|'+text; if (seen.has(key)) return; seen.add(key); const info = CHECK_INFO[id]; items.push({id,group,title:info?.[0] || title,text,action:info?.[1] || (guide && !['paths','recovery'].includes(guide) ? {guide,label:'Guide'} : null)}); };
  // Profile checks follow the draft; readiness repeats them under the generic "Profile compatibility" title.
  for (const i of state?.issues || []) add(i.code, i.severity === 'Error' ? 'fix' : i.severity === 'Warning' ? 'check' : 'ok', 'Profile', i.message);
  for (const c of state?.readiness?.checks || []) if (!(c.title === 'Profile compatibility' && (state.issues || []).some(i => i.code === c.id)))
    add(c.id, c.state === 'Error' ? 'fix' : c.state === 'Pass' ? 'ok' : 'check', c.title, c.detail, c.guide);
  return items;
}
function actionHtml(a) {
  if (!a) return '';
  if (a.reveal) return `<button class="link-button" data-reveal="${escape(a.reveal)}"${a.browse ? ' data-browse-after="1"' : ''}>${escape(a.label)} →</button>`;
  if (a.guide) return `<button class="link-button" data-guide="${escape(a.guide)}">${escape(a.label)} ↗</button>`;
  if (a.review) return `<button class="link-button" data-review="1">${escape(a.label)} →</button>`;
  if (a.action) return `<button class="link-button" data-run="${escape(a.action)}">${escape(a.label)} →</button>`;
  return a.page === page ? '' : `<button class="link-button" data-open-page="${escape(a.page)}">${escape(a.label)} →</button>`;
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
  $('checksNote').textContent = r ? `This PC checked for this draft: ${count('fix')} to fix, ${count('check')} to check yourself.` : 'Settings are checked as you change them. Check this PC adds the headset software, drivers and write access.';
}
document.addEventListener('click', e => {
  const line = e.target.closest('.check-line'); if (line) { const r = line.closest('.check-row'); r.classList.toggle('expanded'); line.setAttribute('aria-expanded',String(r.classList.contains('expanded'))); }
  const guide = e.target.closest('[data-guide]'); if (guide && !busy && guide.closest('#checkGroups')) request('openGuide',{guide:guide.dataset.guide}).catch(showError);
  if (e.target.closest('#checkGroups [data-review]') && !busy) run('preview').catch(()=>{});
  const action = e.target.closest('#checkGroups [data-run]'); if (action && !busy) run(action.dataset.run).catch(()=>{});
});
/** Overview: what Launch DCS will do now, the profile, and the checks in one line. */
function renderDashboard() {
  if (!profile || !state) return;
  const mode = launchState(), items = checkItems(), fix = items.filter(c => c.group === 'fix').length, check = items.filter(c => c.group === 'check').length;
  const titles = {running:'DCS is running', ready:'Ready to fly', changes:'Changes apply at launch', invalid:'Check numeric values'};
  const details = {running:'Close DCS to change the configuration. The flight summary appears here when it exits.',
    ready:state.foveaChanged ? state.foveaChanged : 'The applied profile matches this draft. Launch DCS starts it as it is.', changes:state.appliedDraft ? 'You changed settings since the last launch. Launch DCS applies them.' : 'Nothing is applied yet. Launch DCS backs up the original files, then applies this.',
    invalid:'Correct the marked values before launching.'};
  const chips = [fix ? `<button class="dash-chip fix" data-open-page="diagnostics">${fix} must fix</button>` : '', check ? `<button class="dash-chip check" data-open-page="diagnostics">${check} to check yourself</button>` : '', !fix && !check ? '<button class="dash-chip ok" data-open-page="diagnostics">Nothing to fix</button>' : ''].join('');
  const reset = state.appliedDraft && mode === 'changes' ? '<button class="link-button" data-reset-applied="1">Undo changes</button>' : '';
  $('dashStatus').className = 'dash-status '+mode;
  $('dashStatus').innerHTML = `<span class="dash-dot" aria-hidden="true"></span><div class="dash-main"><b>${titles[mode]}</b><p>${escape(details[mode])}</p><small>${escape(profile.name)}${state.appliedProfile ? ' · applied: '+escape(state.appliedName || state.appliedProfile) : ''}</small></div><div class="dash-side">${chips}${reset}</div>`;
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
const fmt = (v,d = 0) => Number(v).toLocaleString('en-US',{maximumFractionDigits:d,minimumFractionDigits:d});
const share = v => Math.round(v*100)+'%';
function duration(seconds) { const m = Math.floor(seconds/60), h = Math.floor(m/60); return h ? `${h} h ${m%60} min` : m ? `${m} min ${Math.round(seconds%60)} s` : `${Math.round(seconds)} s`; }
function renderFlight() {
  const body = $('flightBody'), f = flight?.flight;
  if (flight?.running) { $('flightTitle').textContent = 'DCS is running'; body.innerHTML = '<p class="fine">The summary appears here after DCS exits.</p>'; return; }
  if (!f) { $('flightTitle').textContent = 'No flight recorded yet'; body.innerHTML = '<p class="fine">After a flight, this shows the frame rate, frame time and what each feature did, read from the logs.</p>'; return; }
  const start = new Date(f.start), seconds = (new Date(f.end) - start) / 1000;
  $('flightTitle').textContent = start.toLocaleString('en-GB',{weekday:'short',day:'numeric',month:'short',hour:'2-digit',minute:'2-digit'}) + ' · ' + duration(seconds) + (f.closed ? '' : ' · DCS did not close normally');
  const rows = [];
  if (f.headset) rows.push(['Headset',`${fmt(f.headset.averageFps,1)} FPS`,`${share(f.headset.atRefreshShare)} of the time at ${fmt(f.headset.compositorFps)} Hz · ${fmt(f.headset.missed)} missed, ${fmt(f.headset.discarded)} discarded`]);
  if (f.framegen) rows.push(['Frame generation',f.framegen.seconds3x && f.framegen.seconds2x ? `2× ${share(1-f.framegen.share3x)} · 3× ${share(f.framegen.share3x)}` : f.framegen.seconds3x ? '3× all flight' : '2× all flight',(f.framegen.adaptive ? 'Auto' : 'Fixed')+(f.framegen.switches ? ` · ${f.framegen.switches} switches` : '')]);
  if (f.frameTime) rows.push(['DCS frame time',`${fmt(f.frameTime.p50Ms,1)} ms`,`median, ${fmt(f.frameTime.p90Ms,1)} ms p90 · CPU or GPU, the longer`]);
  if (f.dlss5) rows.push(['DLSS 5',f.dlss5.state === 'ran' ? 'Ran' : f.dlss5.state === 'failed' ? 'Failed' : 'Not started',f.dlss5.state === 'failed' ? f.dlss5.error : [f.dlss5.version ? 'runtime '+f.dlss5.version : 'no 3D scene reached', f.dlss5.toggles ? `toggled ${f.dlss5.toggles}×, ${f.dlss5.onAtExit ? 'on' : 'off'} at exit` : null].filter(Boolean).join(' · '),f.dlss5.state === 'failed']);
  if (f.boost) rows.push(['CPU Boost',f.boost.state === 'restored' ? 'Restored' : f.boost.state === 'running' ? 'Running' : f.boost.state,`${f.boost.moved} apps moved, ${f.boost.restored} restored`+(f.boost.errors.length ? ` · ${f.boost.errors.length} need admin rights` : ''),f.boost.errors.length > 0]);
  if (f.prefetch) rows.push(['Prefetch fix',f.prefetch.averageCallsPerSecond ? `${fmt(f.prefetch.skippedPercent,1)}% skipped` : 'No calls',f.prefetch.averageCallsPerSecond ? `${fmt(f.prefetch.averageCallsPerSecond/1000,0)}k calls/s average · ${fmt(f.prefetch.peakCallsPerSecond/1000,0)}k peak` : 'Loaded; DCS made no repeated prefetch calls']);
  body.innerHTML = rows.length ? `<dl class="flight-grid">${rows.map(([k,v,d,warn]) => `<div class="flight-item${warn ? ' warn' : ''}"><dt>${k}</dt><dd><b>${escape(v)}</b><small title="${escape(d)}">${escape(d)}</small></dd></div>`).join('')}</dl>` : '<p class="fine">DCS ran, but no headset, frame generation or CPU Boost logs cover this session.</p>';
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
  const info = [['DCS executable',i.dcsExecutable || 'Not detected'],['Saved Games options',i.optionsPath || 'Not detected'],['Pimax runtime',i.pimaxRuntime || 'Not detected'],['SteamVR runtime',i.steamVrRuntime || 'Not detected'],['Active global runtime',i.activeRuntime || 'Not detected'],['Pimax version',i.pimaxVersion || 'Not detected'],['Pimax Play Quad Views',state.pimax?.found ? state.pimax.summary : 'Settings not found'],['DCS process',i.dcsRunning ? 'Running' : 'Closed'],['DCS rendered FPS cap',i.dcsSettings['graphics.maxFPS'] || 'Not detected'],['DCS desktop VSync',i.dcsSettings['graphics.sync'] || 'Not detected'],['Legacy autoexec cap',i.autoexecMaxFps || (i.autoexecPath ? 'Unknown · review file' : 'No autoexec.cfg detected')],['RTSS processes',(i.limiterProcesses || []).join(', ') || 'Not detected · driver caps still need checking']];
  $('inventory').innerHTML = info.map(([k,v]) => `<dt>${k}</dt><dd>${escape(v)}</dd>`).join('');
  $('inventoryObservations').innerHTML = i.observations.map(note => `<p>${escape(note)}</p>`).join('');
  $('inventoryFiles').innerHTML = i.files.map(file => `<div class="file"><b>${escape(file.path)}</b><p>${file.exists ? 'Present' : 'Missing'} · ${escape(file.version || 'No version')}<br><span>${escape(file.sha256 || 'No hash captured')}</span></p></div>`).join('') + i.layers.map(layer => `<div class="file"><b>${escape(layer.name || layer.manifestPath)}</b><p>${escape(layer.scope)} · ${layer.enabled ? 'Enabled' : 'Disabled'} · library ${layer.libraryExists ? 'present' : 'missing'}</p><span>${escape(layer.libraryPath)}</span></div>`).join('');
  if (!i.files.length && !i.layers.length) $('inventoryFiles').innerHTML = '<p class="fine">No file or layer facts were returned by this inventory.</p>';
  $('inventoryTime').textContent = new Date(i.capturedAt).toLocaleTimeString('en-GB');
}
function plannedFilesHtml() {
  return plan ? plan.files.map(f => `<div class="file"><b>${f.replaces ? '~' : '+'} ${escape(f.path)}</b><p>${escape(f.purpose)} <span>· ${f.bytes.toLocaleString('en-US')} bytes</span></p>${(f.luaChanges || []).map(c => `<p>${escape(c.path)}: ${escape(c.previousRaw ?? 'absent')} → ${escape(c.installedRaw)}</p>`).join('')}</div>`).join('') : '<p class="fine">Review files lists every file Launch DCS writes or changes for this profile, with the DCS settings it changes. Nothing is written until you launch.</p>';
}
/** Recovery: one record of the original files DCS Control changed, restored with one button. */
const ORIGINAL_ACTIONS = {restore:'restore the backup', remove:'remove (DCS Control created it)', settings:'set back DCS Control\'s settings only'};
function renderOriginals() {
  const o = state.originals || {count:0, files:[]}, running = Boolean(state.dcsRunning), open = $('originalFiles')?.open;
  const when = o.lastActionAt ? new Date(o.lastActionAt).toLocaleString('en-GB') : null;
  const last = o.lastAction ? `<p class="fine">Last action: ${escape(o.lastAction)}${when ? ' · '+escape(when) : ''}</p>` : '';
  const folder = `<button class="link-button" id="openBackups" title="${escape(o.folder || '')}">Backups folder ↗</button>`;
  const rows = o.files.map(f => `<div class="file original-file"><b>${escape(f.path)}</b><p>${escape(ORIGINAL_ACTIONS[f.action] || f.action)}${f.detail ? ' · '+escape(f.detail) : ''}</p></div>`).join('');
  $('originals').innerHTML = o.count
    ? `<div class="card originals-card" id="originalsCard"><div class="originals-head"><div><span class="tag">${state.appliedProfile ? 'APPLIED: '+escape((state.appliedName || state.appliedProfile).toUpperCase()) : 'CHANGED'}</span><h3>Original files · ${o.count} ${o.count === 1 ? 'file' : 'files'} changed by DCS Control</h3><p>Back to stock DCS puts each one back to what was there before DCS Control first wrote it, whatever it holds now. In options.lua only DCS Control's own settings go back; your other changes stay.</p>${last}</div><button class="button secondary" id="restoreOriginals" ${running || busy ? 'disabled' : ''} ${running ? 'title="Close DCS before restoring"' : ''}>Back to stock DCS</button></div><details id="originalFiles"${open ? ' open' : ''}><summary>What Back to stock DCS does, file by file</summary><div class="file-list">${rows}</div></details><div class="inline-actions">${folder}</div></div>`
    : `<div class="card originals-card empty" id="originalsCard"><div class="empty-state"><h2>No changes to put back</h2><p>DCS Control has not changed any file. Launch DCS (or Apply without launching) backs up each original the first time it writes it.</p>${last}</div><div class="inline-actions">${folder}</div></div>`;
  $('restoreOriginals')?.addEventListener('click',() => run('restore').catch(()=>{}));
  $('openBackups').addEventListener('click',() => request('openBackups').catch(showError));
}
function openSearch() { $('searchDialog').showModal(); $('searchInput').value = ''; renderSearch(); $('searchInput').focus(); }
function renderSearch() {
  const normalize = text => text.toLowerCase().replace(/[^a-z0-9]/g,'');
  const query = normalize($('searchInput').value);
  const shortcuts = [
    ['overview','Features','Turn Engine Optimizations, CPU Boost, Quad Views, DLSS 5 and frame generation on or off in the right panel.','featureList'],
    ['dlss','Advanced image controls','DLSS 5 model corrections: tone, structure, masks, color, depth and motion guides.','neuralAdvanced'],
    ['foveation','Pimax Play focus values','Pimax Play\'s Quad View settings as Pimax Play shows them, and how bundled Quad Views reproduces them. Re-read them.','pimaxFovea'],
    ['boost','What happens at launch','Cores, processes, VRAM to free, the monitor mode and the DCS window the CPU Boost page would change on this PC.','boostPlan'],
    ['diagnostics','Files Launch will write','Review files: every file and DCS setting Launch DCS writes for this profile.','planCard'],
    ['diagnostics','Checks','Must fix, check yourself and OK: what stands between this draft and a flight.','checkGroups'],
    ['overview','Last flight','Headset FPS, frame generation 2×/3×, DCS frame time, DLSS 5, CPU Boost and the prefetch fix from the last flight.','flightCard'],
    ['diagnostics','Diagnostic report','Export detected files, compatibility and backups.','export'],
    ['support','Diagnostic package','One ZIP with the logs and settings needed to understand a problem, for a report.','diagnosticPackage'],
    ['support','DLSS 5 not applied','Why DLSS 5 did not reach the focus area in the last session.','dlssFocus'],
    ['recovery','Stock DCS','Back to stock DCS: put back every file and setting DCS Control changed.','originals']
  ].map(([page,title,description,id]) => ({page,title,description,path:id,element:$(id),input:$(id)}));
  searchResults = [...fields,...shortcuts].filter(f => !(profile?.desktop && VR_PAGES.includes(f.page))).filter(f => !query || normalize(f.title+' '+f.description+' '+f.path+' '+pages.find(p => p[0] === f.page).slice(1,4).join(' ')).includes(query));
  $('searchResults').innerHTML = searchResults.length ? searchResults.map((f,index) => `<button class="search-result" data-index="${index}"><div><b>${escape(f.title)}</b><small>${escape(f.description)}</small></div><span>${f.path === 'featureList' ? 'Right panel' : pages.find(p => p[0] === f.page)[1]} ›</span></button>`).join('') : '<div class="empty-state"><p>No settings match this search.</p></div>';
  $('searchResults').querySelectorAll('button').forEach(b => b.addEventListener('click',() => revealSetting(searchResults[Number(b.dataset.index)])));
}
function revealSetting(f) {
  if (!f) return; if ($('searchDialog').open) $('searchDialog').close(); if (f.path !== 'featureList') navigate(f.page);
  // Focus values that follow Pimax Play are shown in the Pimax block instead of the hidden fields.
  const target = f.element.hidden ? (f.page === 'foveation' ? $('pimaxFovea') : f.element.closest('.card') ?? f.element) : f.element;
  for (let box = target.closest('details'); box; box = box.parentElement.closest('details')) box.open = true;
  target.scrollIntoView({block:'center'}); target.classList.remove('highlight'); void target.offsetWidth; target.classList.add('highlight');
  if (!f.element.hidden && !f.input.disabled) { if (!['INPUT','BUTTON','SELECT','TEXTAREA'].includes(f.input.tagName)) f.input.tabIndex = -1; f.input.focus({preventScroll:true}); }
}
async function initialize() {
  const groups = {engine:'Every pilot',foveation:'VR',setup:'Setup & safety'};
  pages.forEach((p,index) => { if (groups[p[0]]) { const g = document.createElement('div'); g.className = 'rail-group'; g.id = 'railGroup-'+p[0]; g.textContent = groups[p[0]]; $('navigation').append(g); } const b = document.createElement('button'); b.id = 'nav-'+p[0]; b.className = 'nav-item'; b.title = index < 9 ? p[1]+' (Ctrl '+(index+1)+')' : p[1]; b.setAttribute('aria-label',p[1]); b.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="${p[4]}"/></svg><div><b>${p[5] || p[1]}</b><small>${p[3]}</small></div>`; b.addEventListener('click',() => navigate(p[0])); $('navigation').append(b); });
  buildFields(); buildFeatureList(); navigate('overview');
  [['launch','launch'],['apply','apply'],['refresh','refresh'],['importProfile','import'],['saveProfile','save'],['resetNeural','resetNeural'],['export','export'],['prepareSboys','prepareSboys'],['importSboys','importSboys'],['openSboys','openSboys'],['checkReadiness','checkReadiness']].forEach(([id,action]) => $(id).addEventListener('click',() => run(action).catch(()=>{})));
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
  document.addEventListener('keydown',e => { if (e.ctrlKey && (e.code === 'KeyK' || e.key.toLowerCase() === 'k')) { e.preventDefault(); if (!$('searchDialog').open) openSearch(); } const digit = /^Digit([1-9])$/.exec(e.code)?.[1] ?? (/^[1-9]$/.test(e.key) ? e.key : null); if (e.ctrlKey && !e.altKey && digit && Number(digit) <= pages.length) { e.preventDefault(); navigate(pages[Number(digit)-1][0]); } });
  try { sync(await request('ready')); loadFlight(); window.uiReady = true; }
  catch(e) { showError(e); }
}
initialize();
/** Takes a profile and DCS paths chosen elsewhere (guided setup) as the draft, exactly like editing them here. */
function adoptDraft(next) {
  profile = clone(next.profile); invalid.clear(); if (next.dcs !== undefined) dcs = next.dcs ?? ''; if (next.options !== undefined) options = next.options ?? '';
  fields.forEach(f => { if (f.path === 'dcs') setFieldValue(f, dcs); else if (f.path === 'options') setFieldValue(f, options); });
  showProfile(); change(); boostChanged();
}
// Test hooks exercise the same controls and command bridge; the fixture host never starts DCS or opens dialogs.
window.dcsUi = {request,run,saveRuntime,forgetRuntime,adoptDraft,launchState,checkItems,loadFlight,resetToApplied,parseKey,keyLabel,get flight(){return flight;},get savedDraft(){return savedDraftJson;},refreshWorkflow:() => { renderOriginals(); updateWorkflow(); },navigate,draft,sync,showProfile,invalidate:change,fields,withFeatures,featureState,featureNotes,featureSummary,deriveIdentity,FEATURES,rereadPimax,refreshBoostPlan,renderBoostPlan,matchesApplied,effectiveFovea,get boostRequests(){return boostSequence;},get profile(){return profile;},get plan(){return plan;},get invalid(){return invalid;},get state(){return state;}};
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
let detectedProfile = null;
$('detectSetup').addEventListener('click', async () => {
  if (busy) return; setBusy(true);
  try {
    const result = await request('detect', draft()); detectedProfile = result.profile;
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
  profile = clone(detectedProfile); detectedProfile = null; invalid.clear(); showProfile(); change(); boostChanged();
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
$('reviewPlan').addEventListener('click', () => run('preview').catch(()=>{}));
$('openRecovery').addEventListener('click', () => navigate('recovery'));
document.querySelector('.panel-top').addEventListener('scroll', updatePanelScroll, {passive:true});
window.addEventListener('resize', updatePanelScroll);
// Title-bar buttons: the window has no Windows frame, so the page provides them.
document.querySelectorAll('[data-window]').forEach(button => button.addEventListener('click',() => request('window',{op:button.dataset.window}).then(r => { const max = document.querySelector('[data-window="maximize"]'); max.setAttribute('aria-label',r.maximized ? 'Restore' : 'Maximize'); max.title = r.maximized ? 'Restore' : 'Maximize'; }).catch(showError)));

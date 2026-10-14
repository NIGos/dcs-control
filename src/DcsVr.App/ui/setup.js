'use strict';
(() => {
  // Three steps: choose what to run, let the app check it, then fly. Quality values in the current profile are kept.
  // Nothing is written before the last step: its Launch DCS is the panel's Launch DCS (apply, then start DCS).
  const el = id => document.getElementById(id), ui = () => window.dcsUi;
  const labels = ['Choose','Check','Fly'];
  let step = 0, source, candidate, paths, choices, report, checking = false;
  const text = value => String(value ?? '').replace(/[&<>"']/g,c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const opts = (values,current) => values.map(([v,label]) => `<option value="${v}" ${v === current ? 'selected' : ''}>${label}</option>`).join('');
  const activeProfile = () => ui().state?.appliedName || ui().state?.appliedProfile;
  function open() {
    if (!ui()?.profile || document.body.classList.contains('busy')) return;
    if (ui().invalid.size) { el('status').textContent = 'Correct the marked numeric values before opening guided setup.'; return; }
    step = 0; source = structuredClone(ui().profile); candidate = null; report = null;
    const draft = ui().draft(); paths = {dcs:draft.dcs,options:draft.options,neuralRuntimePath:source.neuralRuntimePath || ''};
    const running = ui().state?.activeRoute?.route;
    // The same features as the checklist in the right panel, starting from the current draft.
    choices = {route:source.desktop ? 'Desktop' : running || source.runtime,features:ui().featureState(source),gaze:source.gaze};
    message(''); render(); el('setupDialog').showModal();
  }
  /** The candidate profile for the current choices; quality values are kept, the alternative Quad Views folder is cleared. */
  function plan() {
    const p = ui().withFeatures(source,choices);
    if (p.quadViews !== 'None') p.quadViewsLayerDirectory = null;
    return p;
  }
  /** Sets choices directly (verification and tests): {route, features:{engine,boost,quad,dlss,framegen}, gaze}. */
  function choose(next) { choices = {...choices,...next,features:{...choices.features,...(next.features || {})}}; if (step === 0) render(); }
  function renderChoose() {
    const preview = plan(), notes = ui().featureNotes(preview), on = ui().featureState(preview);
    el('setupFeatures').innerHTML = ui().FEATURES.map(([key,title]) => `<label class="setup-feature ${on[key] ? 'on' : ''}"><input type="checkbox" id="setupFeature-${key}" data-feature="${key}" ${choices.features[key] ? 'checked' : ''}><span><b>${title}</b><small>${text(ui().featureSummary(key,preview))}</small>${notes[key].map(n => `<em class="${n[0]}">${text(n[1])}</em>`).join('')}</span></label>`).join('');
    el('setupGazeRow').hidden = !choices.features.quad || choices.route === 'Desktop';
    // Optimizations only: only Engine Optimizations and CPU Boost are offered.
    for (const [key] of ui().FEATURES) if (!['engine','boost'].includes(key)) el('setupFeature-'+key).closest('.setup-feature').hidden = choices.route === 'Desktop';
  }
  function checkItem(c) {
    return `<article class="readiness-item ${text(c.state)}"><span class="check-mark">${c.state === 'Pass' ? '✓' : c.state === 'Error' ? '!' : 'i'}</span><div><b>${text(c.title)}</b><p>${text(c.detail)}</p>${c.guide && !['paths','recovery'].includes(c.guide) ? `<button class="guide-link" data-guide="${text(c.guide)}">Official setup guide ↗</button>` : ''}</div></article>`;
  }
  function render() {
    el('setupSteps').innerHTML = labels.map((label,i) => `<li class="${i === step ? 'current' : i < step ? 'complete' : ''}" ${i === step ? 'aria-current="step"' : ''}><span>${i < step ? '✓' : '0'+(i+1)}</span><b>${label}</b></li>`).join('');
    el('setupProgress').textContent = `Step ${step+1} of ${labels.length}`;
    el('setupBack').disabled = checking || step === 0;
    el('setupClose').disabled = checking;
    el('setupNext').disabled = checking || (step === 1 && !report?.canPrepare);
    el('setupNext').textContent = step === 0 ? 'Check →' : step === 1 ? 'Continue →' : 'Launch DCS ↗';
    const body = el('setupBody');
    if (step === 0) {
      const live = ui().state?.activeRoute;
      body.innerHTML = `<h3>What do you want to fly with?</h3><p>Your quality settings stay as they are. Only the route and the active features change.</p><div class="setup-grid"><label>How you fly<select id="setupRoute">${opts([['Pimax','Pimax (Pimax Play)'],['SboysSteamVr','Sboys (SteamVR)'],['Desktop','Optimizations only (monitor or any other headset)']],choices.route)}</select><small class="setup-hint">${text(live?.summary || '')}</small></label><label id="setupGazeRow">Focus movement<select id="setupGaze">${opts([['EyeTracked','Eye tracked'],['Fixed','Fixed (centered)']],choices.gaze)}</select></label></div><div class="setup-features" id="setupFeatures" role="group" aria-label="Features"></div>`;
      renderChoose();
      for (const [id,key] of [['setupRoute','route'],['setupGaze','gaze']]) el(id).addEventListener('change',e => { choices[key] = e.target.value; renderChoose(); });
      el('setupFeatures').addEventListener('change',e => { const key = e.target.dataset?.feature; if (!key) return; choices.features[key] = e.target.checked; renderChoose(); el('setupFeature-'+key).focus(); });
    } else if (step === 1) {
      const errors = report?.checks.filter(c => c.state === 'Error') || [], warnings = report?.checks.filter(c => c.state === 'Warning') || [], passed = report?.checks.filter(c => c.state === 'Pass') || [];
      const pathTrouble = errors.some(c => ['game','deployment','neural-runtime'].includes(c.id));
      const sboysTrouble = choices.route === 'SboysSteamVr' && errors.some(c => /sboys/.test(c.id));
      const current = activeProfile(), saved = ui().state?.savedRuntime;
      body.innerHTML = `<h3>${checking ? 'Checking…' : !report ? 'Ready to check' : report.canPrepare ? 'All set' : 'Fix these first'}</h3>`
        + (current && report?.canPrepare ? `<p>Profile <b>${text(current)}</b> is applied now. Launch DCS writes the new one over it; the original files stay backed up.</p>` : '')
        + `<div id="setupChecks">${errors.map(checkItem).join('')}${warnings.map(checkItem).join('')}</div>`
        + (passed.length ? `<details class="setup-passed"><summary>${passed.length} checks passed</summary>${passed.map(checkItem).join('')}</details>` : '')
        + (sboysTrouble ? `<div class="inline-actions"><button id="setupSboys" class="button secondary">Download / verify Sboys</button><button id="setupImportSboys" class="button quiet">Import official ZIP</button></div>` : '')
        + `<details class="setup-files" ${pathTrouble ? 'open' : ''}><summary>Files</summary>` + [['dcs','DCS executable','DCS.exe'],['options','Saved Games settings','options.lua'],...(candidate?.neuralRendering ? [['neuralRuntimePath','NVIDIA DLSS 5 runtime',saved ? `Saved copy · version ${saved.version || 'unknown'} · from ${saved.originalPath}` : 'nvngx_dlssnr.dll']] : [])].map(([key,label,hint]) => `<label class="setup-path">${label}<div><input id="setupPath-${key}" value="${text(paths[key])}" placeholder="${text(hint)}"><button class="button secondary" data-browse="${key}">Browse…</button></div></label>`).join('') + `</details>`
        + `<div class="inline-actions"><button id="setupCheck" class="button quiet" ${checking ? 'disabled' : ''}>Check again</button></div>`;
      el('setupCheck').addEventListener('click',check);
      for (const [id,action] of [['setupSboys','prepareSboys'],['setupImportSboys','importSboys']]) el(id)?.addEventListener('click',async () => {
        checking = true; render(); message(action === 'prepareSboys' ? 'Downloading or checking official Sboys 1.3.0…' : 'Opening the selected file…');
        try { await ui().run(action); message(ui().state.status); } catch(e) { message(e.message); } finally { checking = false; render(); }
        await check();
      });
      body.querySelectorAll('[data-browse]').forEach(button => button.addEventListener('click',async () => {
        const key = button.dataset.browse;
        try {
          const result = await ui().request('browse',{target:key}); if (!result.path) return;
          // The DLSS 5 runtime is verified and kept as the app's copy; the candidate then uses it (empty path).
          let note = '';
          if (key === 'neuralRuntimePath') { note = (await ui().saveRuntime(result.path)).message; paths[key] = ''; }
          else paths[key] = result.path;
          el('setupPath-'+key).value = paths[key]; await check(); if (note) message(note);
        } catch(e) { message(e.message); }
      }));
      body.querySelectorAll('.setup-path input').forEach(input => input.addEventListener('input',() => paths[input.id.slice(10)] = input.value));
      body.querySelectorAll('[data-guide]').forEach(button => button.addEventListener('click',() => ui().request('openGuide',{guide:button.dataset.guide}).catch(e => message(e.message))));
      if (checking) body.querySelectorAll('button,input').forEach(control => control.disabled = true);
    } else {
      const manual = report?.checks.filter(c => c.state === 'Manual') || [], current = activeProfile();
      body.innerHTML = `<h3>Ready to fly</h3><p>${ui().profile?.desktop ? 'Press' : 'Start your headset software, then press'} <b>Launch DCS</b>. It ${current ? `writes over <b>${text(current)}</b>, ` : 'backs up the original files, then '}applies <b>${text(candidate.name)}</b> and starts DCS. <b>Back to stock DCS</b> on the Stock DCS page puts everything back at any time.</p><ol class="flight-checklist">${manual.map(c => `<li><b>${text(c.title)}</b><p>${text(c.detail)}</p></li>`).join('')}</ol><div class="inline-actions"><button id="setupUseDraft" class="button quiet">Keep these settings, don't launch</button></div>`;
      el('setupUseDraft').addEventListener('click',() => { if (checking) return; adopt(); el('setupDialog').close(); });
      if (checking) body.querySelectorAll('button').forEach(control => control.disabled = true);
    }
    body.scrollTop = 0;
  }
  function message(value) { el('setupMessage').textContent = value; }
  function readPaths() { candidate.neuralRuntimePath = paths.neuralRuntimePath.trim() || null; }
  /** The checked candidate and its DCS paths become the main draft (nothing is written). */
  function adopt() { readPaths(); ui().adoptDraft({profile:candidate,dcs:paths.dcs,options:paths.options}); }
  async function check() {
    if (checking) return;
    checking = true; report = null; readPaths(); render(); message('');
    try {
      ui().invalidate();
      const result = await ui().request('checkReadiness',{profile:{...candidate},dcs:paths.dcs,options:paths.options,replace:true}); report = result.readiness;
    } catch(e) { message(e.message); } finally { checking = false; render(); }
  }
  async function next() {
    if (checking) return;
    try {
      if (step === 0) { candidate = plan(); step = 1; message(''); render(); await check(); }
      else if (step === 1 && report?.canPrepare) { readPaths(); step = 2; message(''); }
      else if (step === 2) {
        // The panel's own Launch DCS: restore the applied profile if any, apply this one, then start DCS.
        checking = true; render(); message('Applying and launching…');
        adopt();
        // Another program's dxgi.dll: the same question as the Launch DCS button, which then launches.
        if (ui().askDxgiFirst('launch')) { checking = false; el('setupDialog').close(); return; }
        await ui().run('launch');
        checking = false; el('setupDialog').close();
      }
    } catch(e) { message(e.message); } finally { checking = false; if (el('setupDialog').open) render(); }
  }
  el('guidedSetup').addEventListener('click',open); el('setupClose').addEventListener('click',() => { if (!checking) el('setupDialog').close(); });
  el('setupDialog').addEventListener('cancel',e => { if (checking) e.preventDefault(); });
  el('setupBack').addEventListener('click',() => { if (checking || step === 0) return; step--; if (step === 0) report = null; message(''); render(); });
  el('setupNext').addEventListener('click',next);
  window.setupWizard = {open,next,check,choose,get step(){return step;},get report(){return report;},get candidate(){return candidate;},get choices(){return choices;}};
})();

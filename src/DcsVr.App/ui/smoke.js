window.runWebSmoke = async function() {
  const checks = [], ui = window.dcsUi, $ = id => document.getElementById(id);
  function check(name,value) { if (!value) throw new Error(name); checks.push(name); }
  // Every editable profile control, by path (in page order). Adding or removing a control means adding or removing it
  // here; a failure names the paths that differ. frameGenPipelinedDeepHold is deliberately absent: it follows the
  // Smoothness buffer and stays in the profile file only.
  const PROFILE_FIELDS = [
    'quadViews','gaze','quadTurbo','quadViewsLayerDirectory','foveaSource','quadFocusScale','pimaxPeripheral','pimaxHorizontalFov','pimaxVerticalFov',
    'foveaWidth','foveaHeight','peripheralScale','quadEdgeBlend','quadRoundFocus','quadSharpening','quadSharpenTaper','quadPeripheryContrast','quadSaccadeLead','quadGazeStabilize',
    'neuralRendering','neuralRuntimePath','foveatedDlss','neuralFocusArea','focusDlssPreset','quadNeuralEdgeFade','neuralWorkingScale','neuralBeforeUpscaling','neuralIntensity',
    'neuralStyle','keepOtherDxgi','neuralLocalTone','neuralLocalStructure','neuralSkinStructure','neuralAutomaticMask','neuralUiCorrection','neuralColorStrength',
    'neuralTransferStrength','neuralPaperWhiteScale','neuralDepth','neuralMotionScaleX','neuralMotionScaleY',
    'frameGen','frameGenFactor','frameGenDeepPipeline','frameGenHudSymbology','smoothCursor','nvidiaFlowScale','flowPreset','bidirectionalFlow','frameGenAsyncArm',
    'showOverlay','diagnosticRecorder','diagnosticFrameRecords','fpsLimit','refreshFromHeadset','headsetRefreshHz','renderedFpsCap','disableDcsVSync',
    'cpuBoost','boostDcsPriority','boostMoveVrRuntime','boostMoveBackgroundApps','boostBackgroundApps','boostElevated','freeVram','freeVramApps','freeVramReopen',
    'freeVramForce','pauseTobiiDesktop','smallDcsWindow','lowerMonitor','flightDisplayMode',
    'engineOptimizations','engineGBufferRecorder','engineModelAllocator','engineShaderTimeCache','engineTaskQueueClock','boostPrefetch','engineShadowRecorder',
    'engineShadowInstancing','engineCostWeights','enginePartitionBoost','engineTextureDedupe','engineEffectBufferSkip','enginePlainCounter','engineFrameHeap',
    'engineStateFilter','engineShadowCache','engineBeeps','engineDevMode','engineDevPayloadPath','engineDevIniPath','engineTimerRefreshUs','engineDiagnosticHooks',
    'dcs','options','keepDcsLauncher',
    'eyeDistance','eyeDistanceMm','pupilShift','pupilShiftEyeRadiusMm','pupilShiftVirtualImageM','peripheryStability','peripheryStabilityStrength','farDepth','farDepthPreset',
    'depthBoost','ownAircraftTrueDepth','depthBoostMode','depthBoostRealisticMax','depthBoostFarStartM','depthBoostFarFullM','depthBoostMax','depthBoostNearM',
    'colorMode','panelGamut','colorFixExposureEv','colorFixNightEv','colorFixSaturation','colorFixWarmth','colorFixAdaptBright','colorFixAdaptDark','colorEnhancedExposureEv',
    'colorEnhancedNightEv','colorEnhancedSaturation','colorEnhancedWarmth','colorEnhancedAdaptBright','colorEnhancedAdaptDark','nightVision','nightVisionStrength',
    'glare','glareStrength','nightVisionBlue','nightVisionBlur','nightVisionScotopic','nightVisionPhotopic','glareThresholdEv','glareRadius','glareStreak',
    'gazeDof','gazeDofStrength','gazeDofMaxArcmin','gazeDofSaccadeRelease','gazeDofSettleMs','gazeDofFadeInMs','gazeDofToleranceD','gazeDofFoveaGuardDeg','gazeDofPupilMm',
    'gVision','gVisionStrength','gVisionUseGaze','gVisionRedout','gVisionTolerance','gVisionMode',
    'headsetPanel','diagnosticOverlayAtStart','diagnosticVram','diagnosticOverlayKey','engineToggleKey','neuralToggleKey','pupilShiftToggleKey','depthBoostToggleKey',
    'farDepthToggleKey','colorModeKey','runtimeManifestPath'];
  // Every folded disclosure (details.advanced), named by its id or the first setting inside it.
  const ADVANCED_FOLDS = ['quadViewsLayerDirectory','neuralAdvanced','frameGenAsyncArm','showOverlay','engineTimerRefreshUs','colorFixExposureEv','colorEnhancedExposureEv',
    'nightVisionBlue','gazeDofMaxArcmin','gVisionUseGaze','runtimeManifestPath'];
  /** " (missing: …; extra: …)" between an expected list and the actual one; '' when they hold the same names once each. */
  const listDiff = (expected,actual) => { const missing = expected.filter(x => !actual.includes(x)), extra = actual.filter(x => !expected.includes(x)), twice = actual.filter((x,i) => actual.indexOf(x) !== i);
    return missing.length || extra.length || twice.length || expected.length !== actual.length ? ` (missing: ${missing.join(', ') || 'none'}; extra: ${extra.join(', ') || 'none'}${twice.length ? '; twice: '+twice.join(', ') : ''})` : ''; };
  function edit(path,value) { const f = ui.fields.find(x => x.path === path); if(f.type === 'toggle') f.input.checked = value; else f.input.value = value; f.input.dispatchEvent(new Event(f.type === 'toggle' || f.type === 'select' || f.type === 'hotkey' ? 'change' : 'input',{bubbles:true})); }
  // A key pressed while a capture field records: the page listens on window in the capture phase.
  const press = (keyCode,mods = '') => window.dispatchEvent(new KeyboardEvent('keydown',{keyCode,ctrlKey:mods.includes('c'),altKey:mods.includes('a'),shiftKey:mods.includes('s'),metaKey:mods.includes('w'),bubbles:true,cancelable:true}));
  const sleep = ms => new Promise(r => setTimeout(r,ms));
  // The right panel's checklist, profile name and setup buttons fit without scrolling at the 1320 × 920 verification size.
  const panelFits = () => { const t = document.querySelector('.panel-top'); return innerWidth !== 1320 || innerHeight !== 920 || t.scrollHeight <= t.clientHeight; };
  const waitIdle = async () => { for(let i=0;i<500 && document.body.classList.contains('busy');i++) await new Promise(r => setTimeout(r,20)); };
  // The right-panel checklist, used exactly as a user would: route switch, then one checkbox per feature.
  function select(route,features) {
    if (ui.profile.runtime !== route) document.querySelector(`#routeSwitch [data-route="${route}"]`).click();
    for (const [key] of ui.FEATURES) { const box = $('featureCheck-'+key); if (box.checked !== Boolean(features[key])) { box.checked = Boolean(features[key]); box.dispatchEvent(new Event('change',{bubbles:true})); } }
  }
  try {
    check('Real WebView2 bridge bootstrap with the feature checklist',document.querySelectorAll('#featureList input[type=checkbox]').length === 5 && !$('profilePreset'));
    // Updates and What's new, against the host's fixtures (never the network): the settings record an earlier version,
    // so this version's notes open once at start; GitHub's fixture list offers the next patch version.
    { const until = async test => { for (let i = 0; i < 150 && !test(); i++) await sleep(20); return test(); };
      const notes = $('whatsNewNotes'), next = window.offlineFixture.updateVersion, short = next.split('-')[0];
      check("What's new opens at the first start of a new version with only this version's notes, as text",await until(() => $('whatsNewDialog').open)
        && notes.textContent.includes('Fixture notes') && notes.textContent.includes('<b>not markup</b>') && !notes.querySelector('b b, script, img') && notes.querySelectorAll('li').length === 2 && !notes.textContent.includes('Older notes'));
      $('whatsNewClose').click();
      check("What's new shows once per version",!$('whatsNewDialog').open && !(await ui.request('whatsNew',{startup:true})).show);
      $('appVersionButton').click(); check("The rail's version number opens What's new again",await until(() => $('whatsNewDialog').open) && $('whatsNewTitle').textContent.includes(ui.state.version.split('-')[0]));
      $('whatsNewClose').click();
      check('An available update shows a banner on the Overview and a dot on the version',await until(() => !$('updateBanner').hidden) && $('updateBannerText').textContent === `DCS Control ${short} is available`
        && !$('railUpdate').hidden && ui.updates.update.version === next && ui.updates.update.canInstall && $('updateAuto').checked);
      check('Drafts, malformed tags and older releases are never offered',!ui.updates.update.version.startsWith('99.') && $('updateCheckStatus').textContent.includes(short));
      $('updateNotesOpen').click(); const box = $('updateNotes');
      check("The update's release notes are escaped text with paragraphs and bullets",$('updateDialog').open && !box.querySelector('img, script') && window.notesInjected === undefined
        && box.textContent.includes('<img src=x') && box.querySelector('b')?.textContent === 'Hostile notes check' && box.querySelectorAll('li').length === 3 && box.querySelector('li ul li')
        && box.textContent.includes('First bullet with code') && box.textContent.includes('A link in a paragraph') && !box.textContent.includes('example.invalid'));
      check('An unzipped copy says where the update installs and that this folder can be deleted',!ui.updates.portable || (!$('updatePortable').hidden && $('updatePortable').textContent.includes('can delete')));
      await ui.installUpdate();
      check('A download whose signature does not match is refused',$('updateStatus').textContent.includes('signature does not match') && $('updateStatus').classList.contains('error') && !$('updateInstall').disabled);
      await ui.installUpdate();
      check('A verified download hands over to the setup (offline: not started)',$('updateStatus').textContent.includes("signature was verified") && !$('updateStatus').classList.contains('error') && !$('updateProgressRow').hidden);
      $('updateLater').click();
      $('updateAuto').click();
      check('Turning the update check off is saved and hides the banner',await until(() => ui.updates && !ui.updates.enabled) && $('updateBanner').hidden && $('railUpdate').hidden && !(await ui.request('updateStatus')).enabled);
      $('updateAuto').click(); await until(() => ui.updates.enabled);
      await ui.checkUpdates(true);
      check('Check now finds the update again with the check back on',ui.updates.enabled && !$('updateBanner').hidden && $('updateAuto').checked);
      $('updateHide').click(); check('The banner can be hidden until the next start',$('updateBanner').hidden && !$('railUpdate').hidden); }
    check('One primary action: Launch DCS, with Review files, Apply without launching and Back to stock DCS as links',!$('preview') && !$('heroChips') && $('launch').classList.contains('launch') && $('reviewPlan').classList.contains('link-button') && $('apply').classList.contains('link-button') && $('apply').textContent === 'Apply without launching' && $('openRecovery').classList.contains('link-button') && !document.querySelector('.workflow').textContent.match(/Preview|Apply profile/));
    { const diff = listDiff(PROFILE_FIELDS,ui.fields.map(f => f.path));
      check('All editable profile controls available'+diff,!diff && document.querySelectorAll('#routeSwitch [data-route]').length === 3); }
    check('Quality presets, self-reported limiters and the unused Sboys folder are gone',!$('tuningPreset') && !$('undoTune') && !$('tuningDialog') && !$('budgets') && !['externalLimiter','externalLimiterFps','runtimeReprojection','sboysDriverDirectory'].some(p => ui.fields.some(f => f.path === p)));
    const folds = [...document.querySelectorAll('details.advanced')], foldDiff = listDiff(ADVANCED_FOLDS,folds.map(d => d.id || d.querySelector('.setting')?.id.replace('setting-','') || '(empty fold)'));
    check('Background preparation and per-frame records sit in the Framegen folds; the deep hold has no control',$('setting-frameGenAsyncArm').closest('details') && $('setting-diagnosticFrameRecords').closest('details') && !$('setting-frameGenPipelinedDeepHold') && ui.fields.find(f => f.path === 'frameGenAsyncArm').page === 'framegen' && ui.fields.find(f => f.path === 'diagnosticFrameRecords').page === 'framegen');
    check('Rarely changed settings start folded away'+foldDiff,!foldDiff && folds.every(d => !d.open) &&$('setting-quadViewsLayerDirectory').closest('details') && $('setting-runtimeManifestPath').closest('details') && $('setting-neuralLocalTone').closest('details') && $('setting-diagnosticRecorder').closest('details') && $('setting-engineTimerRefreshUs').closest('details') && $('setting-colorFixSaturation').closest('details') && $('setting-gazeDofSettleMs').closest('details') && !$('setting-colorMode').closest('details'));
    // Pimax Play's page exactly as Pimax Play shows it (labels, order, rounding), from the fixture's Quick 33 % or Fine 33/10 · 33/33.
    const fineFixture = window.offlineFixture.pimaxMode === 'Fine', pimaxShows = () => ui.state.pimax.controls.map(x => x.label+' '+x.value).join(', ');
    const expectedControls = fineFixture
      ? 'Center Resolution 125%, Peripheral Resolution 20%, Top → Center 33%, Bottom → Center 33%, L-Eye Left → Center 33%, L-Eye Right → Center 10%, R-Eye Left → Center 10%, R-Eye Right → Center 33%, Transition Mode Alpha, Transition Range 5%, Alpha 50%'
      : 'Center Resolution 125%, Peripheral Resolution 20%, Horizontal FOV 33%, Vertical FOV 33%, Vertical Offset 0, Transition Mode Alpha';
    const expectedShort = fineFixture ? 'Fine 33/10 · 33/33 · 125% · 20%' : 'Quick 33% × 33% · 125% · 20%', focusW = fineFixture ? .57 : .67, focusH = fineFixture ? .34 : .67;
    check('Pimax Play values are shown exactly as Pimax Play shows them ('+window.offlineFixture.pimaxMode+')',ui.state.pimax.found && ui.state.pimax.mode === window.offlineFixture.pimaxMode && pimaxShows() === expectedControls && ui.state.pimax.short === expectedShort && !ui.state.pimax.mismatch);
    check('Pimax Play '+expectedShort+' converts to the focus Pimax renders at 1.125x',Math.abs(ui.state.pimax.converted.width - focusW) < 1e-6 && Math.abs(ui.state.pimax.converted.height - focusH) < 1e-6 && ui.state.pimax.converted.focusScale === 1.125 && Math.abs(ui.state.pimax.converted.peripheralScale - .1919) < 1e-6);
    check('The focus size in pixels matches what Pimax logs at 5424 x 5356',ui.state.pimax.eyeResolution?.width === 5424 && ui.state.pimax.eyeResolution.focusWidth === (fineFixture ? 3478 : 4088) && ui.state.pimax.eyeResolution.focusHeight === (fineFixture ? 2048 : 4038));
    { const before = ui.profile.runtime; edit('quadViews','PimaxNative'); document.querySelector('#routeSwitch [data-route="SboysSteamVr"]').click(); check('Switching to Sboys replaces Pimax native Quad Views',ui.profile.runtime === 'SboysSteamVr' && ui.profile.quadViews === 'QuadViewsFoveated');
      check('The Sboys driver card shows on the Sboys route',!$('sboysCard').hidden); document.querySelector('#routeSwitch [data-route="Pimax"]').click(); check('The Sboys driver card is hidden on the Pimax route',$('sboysCard').hidden); document.querySelector(`#routeSwitch [data-route="${before}"]`).click(); }
    { const before = {runtime:ui.profile.runtime,quadViews:ui.profile.quadViews,frameGen:ui.profile.frameGen,neural:ui.profile.neuralRendering};
      document.querySelector('#routeSwitch [data-route="Desktop"]').click();
      ui.navigate('framegen'); const vrPageOpened = !$('page-framegen').hidden; ui.navigate('overview');
      check('Optimizations only keeps the VR settings for later, hides their rows and pages and never opens them',!vrPageOpened && ui.profile.desktop === true && ui.profile.quadViews === before.quadViews && ui.profile.frameGen === before.frameGen && $('feature-quad').hidden && $('feature-dlss').hidden && $('feature-framegen').hidden && !$('feature-boost').hidden && $('nav-foveation').hidden && $('nav-framegen').hidden && !$('nav-engine').hidden && ui.profile.name === 'Optimizations only' && ui.profile.id === 'desktop-optimizations');
      document.querySelector(`#routeSwitch [data-route="${before.runtime}"]`).click();
      check('Pimax or Sboys after Optimizations only brings the VR features back',ui.profile.desktop === false && ui.profile.runtime === before.runtime && ui.profile.quadViews === before.quadViews && ui.profile.frameGen === before.frameGen && ui.profile.neuralRendering === before.neural && !$('feature-quad').hidden && !$('nav-foveation').hidden); }
    check('Legacy defaults preserve the DCS frame limit and VSync',ui.profile.fpsLimit === 'Preserve' && !ui.profile.disableDcsVSync);
    for(const id of ['overview','guide','foveation','dlss','framegen','pupil','visual','boost','engine','setup','diagnostics','support','recovery']) { ui.navigate(id); check('Navigate '+id,!$('page-'+id).hidden && $('nav-'+id).getAttribute('aria-current') === 'page'); }
    check('The frame generation page is called Framegen',$('nav-framegen').textContent.startsWith('Framegen') && $('pageTitle').textContent !== 'Frame rate' && (ui.navigate('framegen'), $('pageTitle').textContent === 'Framegen'));
    { ui.navigate('guide'); const cards = [...document.querySelectorAll('#featureGuide .guide-card')];
      check('The Feature guide has a card per feature, each with its benefit and an Open button that leads to its page',cards.length >= 10 && cards.every(c => c.querySelector('.guide-impact') && c.querySelector('[data-guide-page]'))
        && (cards[0].querySelector('[data-guide-page]').click(), page === 'engine' || $('nav-engine').classList.contains('selected'))); ui.navigate('overview'); }
    // Overview: launch status, the four feature tiles and the last flight, read from the fixture's logs.
    ui.navigate('overview');
    { const result = await ui.request('lastFlight'), f = result.flight;
      check('Last flight is read from the logs: headset frames, 2x/3x share, frame time, DLSS 5, CPU Boost and the terrain prefetch fix',!result.running && f && f.closed && f.dcsPid === 4242 && f.headset.seconds > 60 && f.headset.averageFps > 80 && f.framegen.seconds2x > 0 && f.framegen.seconds3x > 0 && f.frameTime.p90Ms >= f.frameTime.p50Ms && f.dlss5.state === 'ran' && f.dlss5.version === '310.8' && f.dlss5.toggles === 2 && f.dlss5.onAtExit === true && !f.dlss5.error && f.boost.moved === 3 && f.boost.errors.length === 1 && f.prefetch.skippedPercent > 90);
      ui.loadFlight(0); await sleep(150);
      check('Overview shows the last flight as one compact card',$('flightBody').querySelectorAll('.flight-item').length === 6 && $('flightTitle').textContent.includes('min') && $('flightBody').textContent.includes('3×') && $('flightBody').textContent.includes('310.8'));
      check('Overview opens with the launch status and the checks in one line, without a separate list of problems',!$('overviewNotes') && $('dashStatus').querySelector('.dash-main b') && $('dashStatus').querySelector('.dash-chip') && $('pipeline').querySelectorAll('.stage').length === 5); }
    edit('fpsLimit','MatchRefresh'); edit('headsetRefreshHz',90); edit('frameGen','Nvidia');
    check('OFXR matching mode shows 45 rendered FPS for 90 Hz',$('cadenceMetrics').textContent.includes('45 FPS') && $('cadenceMetrics').textContent.includes('Up to 90 FPS'));
    edit('frameGen','Off'); check('Turning framegen off restores a 90 FPS cadence',$('cadenceMetrics').textContent.includes('90 FPS') && !$('cadenceMetrics').textContent.includes('45 FPS'));
    edit('fpsLimit','Custom'); edit('renderedFpsCap',30); edit('frameGen','Nvidia'); check('Low cap exposes output shortfall',$('cadenceWarnings').textContent.includes('below'));
    check('Only the real cadence problem is warned about',$('cadenceWarnings').children.length === 1);
    edit('fpsLimit','Preserve');
    { const before = ui.profile.frameGenFactor; edit('frameGenFactor',2); const deep = ui.fields.find(f => f.path === 'frameGenDeepPipeline');
      check('Smoothness buffer sits under Frame generation, on by default and editable in 2x',deep.page === 'framegen' && ui.profile.frameGenDeepPipeline === true && !$('input-frameGenDeepPipeline').disabled);
      edit('frameGenDeepPipeline',false); check('Smoothness buffer off is kept in the profile',ui.profile.frameGenDeepPipeline === false);
      edit('frameGenDeepPipeline',true); edit('frameGenFactor',3); check('Smoothness buffer stays editable in 3x',!$('input-frameGenDeepPipeline').disabled && $('setting-frameGenDeepPipeline').textContent.includes('only when needed'));
      edit('frameGenFactor',0); check('Smoothness buffer stays editable in Auto',!$('input-frameGenDeepPipeline').disabled);
      edit('frameGenFactor',before); }
    ui.navigate('dlss'); check('Inactive neural controls disabled', $('input-neuralIntensity').disabled);
    edit('foveaWidth',''); check('Incomplete numeric entry blocks Launch, Apply and Review',$('launch').disabled && $('apply').disabled && $('reviewPlan').disabled && ui.invalid.has('foveaWidth') && $('statusTitle').textContent === 'Check numeric values');
    edit('foveaWidth','1.1'); check('Out-of-range numeric entry blocks save',$('saveProfile').disabled);
    edit('foveaWidth','0.612345678901234'); check('Exact numeric value preserved',ui.profile.foveaWidth === .612345678901234 && !ui.invalid.size);
    $('searchOpen').click(); $('searchInput').value = 'pimax play focus'; $('searchInput').dispatchEvent(new Event('input')); check('Search finds the Pimax Play focus values',[...$('searchResults').querySelectorAll('b')].some(b => b.textContent === 'Pimax Play focus values'));
    $('searchInput').value = 'happens at launch'; $('searchInput').dispatchEvent(new Event('input')); check('Search finds the CPU Boost plan',[...$('searchResults').querySelectorAll('b')].some(b => b.textContent === 'What happens at launch'));
    $('searchInput').value = 'paper white'; $('searchInput').dispatchEvent(new Event('input')); check('Search filters settings',$('searchResults').querySelectorAll('button').length === 1); $('searchResults').querySelector('button').click(); check('Search opens a folded DLSS 5 setting',!$('page-dlss').hidden && $('neuralAdvanced').open);
    $('neuralAdvanced').open = false;
    check('Motion follows Windows only: no Reduce motion toggle, a prefers-reduced-motion rule instead',!$('reducedMotion') && !document.querySelector('.motion') && [...document.styleSheets].some(sheet => [...sheet.cssRules].some(r => r.media?.mediaText?.includes('prefers-reduced-motion'))));
    let preferencesRefused = false; try { await ui.request('preferences',{reducedMotion:true}); } catch(e) { preferencesRefused = /Unknown interface command/.test(e.message); } check('The motion preference command is gone',preferencesRefused);
    select('Pimax',{quad:true,dlss:true,framegen:true,boost:true});
    check('Smoke runs at the 1320 x 920 verification size',innerWidth === 1320 && innerHeight === 920);
    const panelSizes = () => { const t = document.querySelector('.panel-top'); return ` (content ${t.scrollHeight} px in ${t.clientHeight} px; ` + [...t.children].map(c => (c.className || c.tagName)+' '+Math.round(c.getBoundingClientRect().height)).join(', ') + `; footer ${Math.round(document.querySelector('.workflow').getBoundingClientRect().height)})`; };
    check('Right panel fits at 1320 x 920 with every feature on and its notes, without a scroll cue'+(panelFits() ? '' : panelSizes()),panelFits() && !document.querySelector('.workflow').classList.contains('more-above') && $('detectSetup').getBoundingClientRect().bottom <= document.querySelector('.workflow').getBoundingClientRect().top);
    check('Each feature is one row: checkbox, name, one-line state and a chevron',ui.FEATURES.every(([key]) => { const row = $('feature-'+key), summary = $('featureSummary-'+key); return row.querySelector('input[type=checkbox]') && row.querySelector('.feature-open') && summary.closest('label') && getComputedStyle(summary).whiteSpace === 'nowrap' && summary.title === summary.textContent; }));
    check('Dependency notes are short and only where they apply',[...document.querySelectorAll('#featureList .feature-note span')].every(n => n.textContent.length <= 40) && $('featureNotes-framegen').textContent === '' && $('featureNotes-boost').textContent === '');
    // In-flight keys: click the field, press the combination.
    ui.navigate('dlss'); { const nrKey = $('input-neuralToggleKey'), diagKey = $('input-diagnosticOverlayKey'), before = ui.profile.neuralToggleKey;
      check('The DLSS 5 key is a capture field showing the key as it reads',ui.fields.find(f => f.path === 'neuralToggleKey').type === 'hotkey' && nrKey.tagName === 'BUTTON' && nrKey.textContent === ui.keyLabel(ui.parseKey(before)));
      nrKey.click(); check('Clicking the field starts recording',nrKey.classList.contains('capturing') && $('keynote-neuralToggleKey').textContent.includes('Esc'));
      press(17,'cs'); check('Held modifiers show while recording',nrKey.textContent === 'Ctrl+Shift+…');
      press(75); check('A letter without a modifier is refused and recording goes on',nrKey.classList.contains('capturing') && $('keynote-neuralToggleKey').textContent.includes('needs Ctrl') && ui.profile.neuralToggleKey === before);
      press(115,'a'); check('A Windows combination is refused',$('keynote-neuralToggleKey').textContent.includes('used by Windows'));
      press(75,'cw'); check('Any combination with the Windows key is refused',nrKey.classList.contains('capturing') && $('keynote-neuralToggleKey').textContent.includes('Windows key cannot') && ui.profile.neuralToggleKey === before);
      for (const code of [0,255,229]) { press(code,'c'); check('A key without a usable key code is refused ('+code+')',nrKey.classList.contains('capturing') && $('keynote-neuralToggleKey').textContent.includes('no usable key code') && ui.profile.neuralToggleKey === before); }
      press(27); check('Esc cancels and keeps the key',!nrKey.classList.contains('capturing') && ui.profile.neuralToggleKey === before);
      nrKey.click(); press(75,'ca'); check('Ctrl+Alt+K is recorded as virtual-key:modifiers',!nrKey.classList.contains('capturing') && ui.profile.neuralToggleKey === '75:3' && nrKey.textContent === 'Ctrl+Alt+K' && $('keynote-neuralToggleKey').textContent === '');
      diagKey.click(); press(75,'ca');
      check('The other in-flight key cannot take the same combination',diagKey.classList.contains('capturing') && $('keynote-diagnosticOverlayKey').textContent.includes('already the DLSS 5 toggle key') && ui.profile.diagnosticOverlayKey !== '75:3');
      press(27);
      nrKey.click(); press(76,'c'); check('A DCS default binding is accepted with a note',ui.profile.neuralToggleKey === '76:1' && $('keynote-neuralToggleKey').classList.contains('warn') && $('keynote-neuralToggleKey').textContent.includes('DCS also uses Ctrl+L'));
      nrKey.click(); press(8); check('Backspace clears the key (Off)',ui.profile.neuralToggleKey === 'Off' && nrKey.textContent === 'Off');
      ui.profile.neuralToggleKey = 'Ctrl+Alt+Shift+F10'; ui.invalidate(); ui.showProfile();
      check('A key saved by an earlier version still shows and works',nrKey.textContent === 'Ctrl+Alt+Shift+F10' && ui.parseKey(ui.profile.neuralToggleKey).join(':') === '121:7');
      nrKey.click(); press(121,'cas'); check('Recording the same combination keeps the saved spelling',ui.profile.neuralToggleKey === 'Ctrl+Alt+Shift+F10');
      ui.profile.neuralToggleKey = before; ui.invalidate(); ui.showProfile(); }
    { const engine = ['engineOptimizations','engineShaderTimeCache','enginePartitionBoost','engineTimerRefreshUs','engineDiagnosticHooks'].map(p => ui.fields.find(f => f.path === p));
      check('DCS engine optimizations sit on their own page, off by default, both measured optimizations on, the rest under Advanced',engine.every(f => f && f.page === 'engine') && ui.profile.engineOptimizations === false && ui.profile.engineShaderTimeCache === true && ui.profile.enginePartitionBoost === true && ui.profile.engineShadowInstancing === true && ui.profile.engineFrameHeap === true && ui.profile.engineTaskQueueClock === true && ui.profile.engineCostWeights === true && ui.profile.engineBeeps === true && ui.profile.engineModelAllocator === true && ui.profile.engineTextureDedupe === true && ui.profile.engineEffectBufferSkip === true && ui.profile.engineStateFilter === true && ui.profile.engineShadowRecorder === true && ui.profile.engineGBufferRecorder === true && ui.profile.enginePlainCounter === true && ui.profile.engineTimerRefreshUs === 1000 && ui.profile.engineDiagnosticHooks === false && !$('setting-engineShaderTimeCache').closest('details') && $('setting-engineTimerRefreshUs').closest('details') && $('setting-engineDiagnosticHooks').closest('details') && !$('setting-engineDevMode').closest('details') && ui.profile.engineDevMode === false && $('engineSuite').disabled); }
    { const f = ui.fields.find(f => f.path === 'boostPrefetch');
      check('The terrain prefetch fix is an Engine Optimizations field, Skip by default and gone from CPU Boost',f && f.page === 'engine' && ui.profile.boostPrefetch === 'Skip' && !$('setting-boostPrefetch').closest('details') && !document.querySelector('#page-boost [id="setting-boostPrefetch"]')); }
    { const was = ui.profile.engineOptimizations; edit('engineOptimizations',true); ui.navigate('engine'); const k = $('input-engineToggleKey');
      check('The engine optimizations switch is a capture field, Alt+Shift+F11 by default',ui.fields.find(f => f.path === 'engineToggleKey').type === 'hotkey' && ui.profile.engineToggleKey === '122:6' && k.textContent === 'Alt+Shift+F11');
      k.click(); press(120,'ca'); check('The module test keys (Ctrl+Alt+F9 to F12) are refused for the switch',k.classList.contains('capturing') && $('keynote-engineToggleKey').textContent.includes('reserved by Engine Optimizations') && ui.profile.engineToggleKey === '122:6');
      press(27); edit('engineOptimizations',was); }
    { const was = {pupilShift:ui.profile.pupilShift, quadViews:ui.profile.quadViews, gaze:ui.profile.gaze, engine:ui.profile.engineOptimizations}; edit('engineOptimizations',true);
      edit('pupilShift',true); edit('quadViews','PimaxNative'); edit('gaze','EyeTracked'); ui.navigate('pupil'); const k = $('input-pupilShiftToggleKey');
      check('Pupil shift has its own page, marked experimental, with its switch as a capture field (Ctrl+Shift+F10)',!$('page-pupil').hidden && $('pupilControls').textContent.includes('EXPERIMENTAL') && $('pupilNeeds').hidden && ui.fields.find(f => f.path === 'pupilShiftToggleKey').type === 'hotkey' && ui.profile.pupilShiftToggleKey === '121:5' && k.textContent === 'Ctrl+Shift+F10');
      k.click(); press(122,'as'); check('The pupil shift switch refuses a key another switch uses',k.classList.contains('capturing') && ui.profile.pupilShiftToggleKey === '121:5'); press(27);
      edit('gaze','Fixed'); check('Without an eye-tracked gaze the page says why and links to the setting',!$('pupilNeeds').hidden && $('pupilNeeds').textContent.includes('Focus movement') && ($('pupilFix').click(), !$('page-foveation').hidden));
      edit('gaze',was.gaze); edit('quadViews',was.quadViews); edit('pupilShift',was.pupilShift); edit('engineOptimizations',was.engine); ui.navigate('pupil');
      await new Promise(r => setTimeout(r,300)); check('The pupil shift status reads the log through the bridge',!!$('pupilStatus').querySelector('.engine-state')); }
    { const was = {on:ui.profile.peripheryStability, quadViews:ui.profile.quadViews, strength:ui.profile.peripheryStabilityStrength, neural:ui.profile.neuralRendering, foveated:ui.profile.foveatedDlss}; ui.navigate('foveation');
      const f = ui.fields.find(x => x.path === 'peripheryStabilityStrength');
      check('Periphery stability is a card on the Quad Views page, off by default, marked experimental',f.page === 'foveation' && $('stabilityControls').textContent.includes('Periphery stability') && $('stabilityControls').textContent.includes('EXPERIMENTAL') && (was.on || f.input.disabled) && ui.fields.find(x => x.path === 'peripheryStability').type === 'toggle');
      edit('peripheryStability',true); edit('quadViews','QuadViewsFoveated'); edit('peripheryStabilityStrength','0.25');
      check('With the bundled Quad Views it needs nothing and its strength is editable',$('stabilityNeeds').hidden && !f.input.disabled && ui.profile.peripheryStabilityStrength === 0.25);
      // Pimax native needs DLSS 5 and Foveated DLSS off (they force the bundled provider).
      edit('neuralRendering',false); edit('foveatedDlss',false); edit('quadViews','PimaxNative'); check('With Pimax native Quad Views the page says it is not applied and links to the provider',!$('stabilityNeeds').hidden && $('stabilityNeeds').textContent.includes('bundled Quad Views only') && ($('stabilityFix').click(), !$('page-foveation').hidden));
      edit('quadViews',was.quadViews); edit('neuralRendering',was.neural); edit('foveatedDlss',was.foveated); edit('peripheryStabilityStrength',String(was.strength)); edit('peripheryStability',was.on); ui.navigate('foveation'); }
    // Depth boost: a card of its own on the Perception page, off by default, with its own switch; it needs the same gaze.
    { const was = {pupilShift:ui.profile.pupilShift, depthBoost:ui.profile.depthBoost, quadViews:ui.profile.quadViews, gaze:ui.profile.gaze, engine:ui.profile.engineOptimizations};
      check('Depth boost is off by default and its settings wait for it',ui.profile.depthBoost === false && ui.profile.depthBoostMax === 1.4 && ui.profile.depthBoostMode === 'Realistic' && ui.profile.depthBoostRealisticMax === 1.25 && $('input-depthBoostMax').disabled && $('setting-depthBoostMax').classList.contains('inactive') && $('input-depthBoostMode').disabled);
      edit('engineOptimizations',true); edit('quadViews','PimaxNative'); edit('gaze','EyeTracked'); edit('pupilShift',true); edit('depthBoost',true); ui.navigate('pupil');
      const k = $('input-depthBoostToggleKey');
      check('Depth boost has its card on the Perception page, marked experimental, with its switch as a capture field (Alt+Shift+F10)',!$('page-pupil').hidden && $('depthControls')
        && $('depthControls').closest('#page-pupil') && $('depthControls').textContent.includes('EXPERIMENTAL') && $('depthControls').textContent.includes('landings')
        && ui.fields.find(f => f.path === 'depthBoostToggleKey').type === 'hotkey' && ui.profile.depthBoostToggleKey === '121:6' && k.textContent === 'Alt+Shift+F10' && !$('input-depthBoostRealisticMax').disabled && $('input-depthBoostMax').disabled);
      k.click(); press(121,'cs'); check('The Depth boost switch refuses the pupil shift switch',k.classList.contains('capturing') && ui.profile.depthBoostToggleKey === '121:6'); press(27);
      check('No transition choice or transition key: the dual camera only moves the depth smoothly',!$('input-depthBoostTransition') && !$('input-depthBoostTransitionKey') && !$('input-ownAircraftTrueDepth').disabled && ui.profile.ownAircraftTrueDepth === true);
      check('Realistic by default: its strength and distances, not Enhanced\'s strength',!$('input-depthBoostRealisticMax').disabled && !$('input-depthBoostFarStartM').disabled && $('input-depthBoostMax').disabled);
      edit('depthBoostRealisticMax','1.6'); check('A Realistic strength beyond ×1.5 is refused',$('setting-depthBoostRealisticMax').classList.contains('invalid')); edit('depthBoostRealisticMax','1.25');
      edit('depthBoostMode','Enhanced'); check('Enhanced: its own strength, Realistic\'s settings wait',!$('input-depthBoostMax').disabled && $('input-depthBoostRealisticMax').disabled && $('input-depthBoostFarFullM').disabled);
      edit('depthBoostMax','2.5'); check('A strength beyond ×2 is refused',$('setting-depthBoostMax').classList.contains('invalid')); edit('depthBoostMax','1.4'); edit('depthBoostMode','Realistic');
      edit('pupilShift',false); edit('gaze','Fixed'); check('Without an eye-tracked gaze the page says Depth boost needs it',!$('pupilNeeds').hidden && $('pupilNeeds').textContent.includes('Depth boost needs'));
      edit('gaze',was.gaze); edit('quadViews',was.quadViews); edit('depthBoost',was.depthBoost); edit('pupilShift',was.pupilShift); edit('engineOptimizations',was.engine); ui.navigate('pupil'); }
    ui.navigate('support'); await new Promise(r => setTimeout(r,300));
    check('Troubleshooting explains DLSS 5 on the focus area from the last session and offers the diagnostic package',$('dlssFocus').textContent.includes('No session') && $('diagnosticPackage').textContent.startsWith('Create diagnostic package') && $('page-support').textContent.includes('<user>'));
    { const cursor = ui.fields.find(f => f.path === 'smoothCursor');
      check('Smooth mouse cursor sits under Frame generation, off by default, and says the click does not move',cursor.page === 'framegen' && cursor.type === 'toggle' && ui.profile.smoothCursor === false && !$('setting-smoothCursor').closest('details') && $('setting-smoothCursor').textContent.includes("Where you click doesn't change")); }
    { const vram = ui.fields.find(f => f.path === 'diagnosticVram');
      check('The VRAM counter sits with the in-headset panel settings on the OSD page, off by default',vram.page === 'osd' && vram.type === 'toggle' && $('setting-diagnosticVram').closest('.card') === $('setting-headsetPanel').closest('.card') && ui.profile.diagnosticVram === false); }
    // The draft on screen is saved a moment after an edit, so it survives an app restart.
    edit('quadSharpening',.62); await sleep(900); check('The draft is saved after an edit',ui.savedDraft === JSON.stringify(ui.draft()));
    check('Profile name follows the route and the checked features',ui.profile.name === 'Pimax · Quad Views + DLSS 5 + Frame generation + CPU Boost' && ui.profile.id === 'pimax-qv-dlss5-fg-boost' && $('profileName').textContent === ui.profile.name);
    check('Overview shows a CPU Boost tile next to the other features',$('pipeline').querySelectorAll('.stage').length === 5 && $('pipeline').querySelector('[data-page=boost]').classList.contains('on'));
    edit('quadViews','PimaxNative');
    check('DLSS 5 with Quad Views keeps the bundled provider and the focus adapter, explained in one short note',ui.profile.quadViews === 'QuadViewsFoveated' && ui.profile.quadFocusAdapter && $('featureNotes-quad').textContent.includes("can't host") && $('featureNotes-quad').textContent.length < 60 && $('input-quadViews').querySelector('[value=PimaxNative]').disabled);
    check('DLSS 5 without its runtime file asks for it inline',ui.profile.neuralRuntimePath || $('featureNotes-dlss').textContent.includes('Select file'));
    edit('foveaSource','PimaxPlay'); ui.navigate('foveation'); await waitIdle();
    { const box = $('pimaxFovea'), shown = [...box.querySelectorAll('.pimax-controls dt')].map((dt,i) => dt.textContent+' '+box.querySelectorAll('.pimax-controls dd')[i].textContent).join(', ');
      check('Pimax Play values replace the focus fields, read-only, in Pimax Play units',['pimaxHorizontalFov','pimaxVerticalFov','quadFocusScale','pimaxPeripheral','foveaWidth','foveaHeight','peripheralScale'].every(f => $('setting-'+f).hidden) && !box.hidden && box.textContent.includes(window.offlineFixture.pimaxMode+' mode') && shown === expectedControls && $('featureSummary-quad').textContent.includes("from Pimax Play's settings: "+expectedShort));
      check('The conversion is folded away under its own heading',!box.textContent.includes('WRITES') && box.querySelector('details.pimax-details') && !box.querySelector('details.pimax-details').open && box.querySelector('details.pimax-details summary').textContent === 'How bundled Quad Views reproduces this' && box.querySelector('details.pimax-details').textContent.includes('same % for different sizes') && box.querySelector('details.pimax-details').textContent.includes((fineFixture ? '3478 × 2048' : '4088 × 4038')+' px')); }
    // This profile: Pimax Quick units, converted exactly like Pimax Quick, without touching values the user does not edit.
    { const before = {w:ui.profile.foveaWidth,h:ui.profile.foveaHeight,c:ui.profile.quadFocusScale,p:ui.profile.peripheralScale};
      edit('foveaSource','Profile'); check('This profile shows the editable focus fields in Pimax Quick units',!$('setting-pimaxHorizontalFov').hidden && !$('setting-quadFocusScale').hidden && $('setting-foveaWidth').hidden && $('pimaxFovea').hidden && $('input-pimaxHorizontalFov').value === String(Math.round((1-before.w)*100)));
      check('Showing a profile in Pimax units changes none of its values',ui.profile.foveaWidth === before.w && ui.profile.foveaHeight === before.h && ui.profile.quadFocusScale === before.c && ui.profile.peripheralScale === before.p);
      edit('pimaxHorizontalFov',33); edit('pimaxVerticalFov',33); edit('quadFocusScale',125); edit('pimaxPeripheral',20);
      check('Quick 33% × 33% · 125% · 20% maps exactly like Pimax Quick',ui.profile.foveaWidth === .67 && ui.profile.foveaHeight === .67 && ui.profile.quadFocusScale === 1.125 && ui.profile.peripheralScale === .1919 && $('featureSummary-quad').textContent.endsWith('33% × 33% · 125% · 20%'));
      edit('pimaxHorizontalFov',43); edit('pimaxVerticalFov',66); check('Fine 33/10 · 33/33 is Quick 43% × 66% here',ui.profile.foveaWidth === .57 && ui.profile.foveaHeight === .34);
      edit('pimaxHorizontalFov',5); check('The 90% cap is stated in Pimax units',$('help-pimaxHorizontalFov').textContent.includes('5% renders as 10%') && ui.effectiveFovea(ui.profile).w === .9);
      Object.assign(ui.profile,{foveaWidth:before.w,foveaHeight:before.h,quadFocusScale:before.c,peripheralScale:before.p}); ui.invalidate(); ui.showProfile(); edit('foveaSource','PimaxPlay'); }
    const vramDefaults = ui.profile.freeVramApps.join('\n');
    check('Flight helpers start off, with a default list that leaves voice chat and recording alone',!ui.profile.freeVram && !ui.profile.smallDcsWindow && !ui.profile.lowerMonitor && !ui.profile.freeVramForce && ui.profile.freeVramApps.includes('NVIDIA Overlay') && !ui.profile.freeVramApps.some(n => /discord|obs/i.test(n)) && !ui.fields.some(f => f.path === 'boostCloseApps'));
    edit('freeVram',true); edit('freeVramApps','notepad\nsteam'); await ui.run('refresh');
    check('Free VRAM refuses to close a process that starts DCS',ui.state.issues.some(i => i.code === 'boost-launcher' && i.severity === 'Error'));
    ui.navigate('diagnostics');
    { const fix = [...$('checkGroups').querySelectorAll('.check-fix .check-row')], row = fix.find(r => r.textContent.includes('CPU Boost app lists'));
      check('Checks lists the problem once under Must fix, one line with a link to the fix',row && row.querySelector('[data-open-page=boost]') && getComputedStyle(row.querySelector('.check-line')).whiteSpace === 'nowrap' && fix.filter(r => r.textContent.includes('CPU Boost app lists')).length === 1);
      check('Checks groups Must fix, Check yourself and OK, with OK folded',$('checkGroups').querySelector('.check-group.check-fix h3').textContent.includes('Must fix') && $('checkGroups').querySelector('.check-group.check-check h3').textContent.includes('Check yourself') && (!$('checkGroups').querySelector('details.check-ok') || !$('checkGroups').querySelector('details.check-ok').open));
      row.querySelector('.check-line').click(); check('A click shows the whole text',row.classList.contains('expanded'));
      ui.navigate('overview'); check('Overview counts what to fix and links to Checks',$('dashStatus').querySelector('.dash-chip.fix[data-open-page=diagnostics]')?.textContent.includes('must fix')); }
    edit('freeVramApps',vramDefaults); edit('freeVram',false); const boostPlan = await ui.request('boostPlan',ui.draft());
    // What happens at launch is fetched again for Boost and route edits, not for other edits.
    ui.navigate('boost'); await sleep(450); let requests = ui.boostRequests; edit('quadSharpening',.61); edit('neuralIntensity',.8); await sleep(450);
    check('Non-Boost edits do not refetch the Boost plan',ui.boostRequests === requests);
    edit('boostDcsPriority','High'); await sleep(450); check('Boost edits refetch the Boost plan once',ui.boostRequests === requests + 1); edit('boostDcsPriority','AboveNormal'); await sleep(450);
    const [planA, planB] = await Promise.all([ui.request('boostPlan',ui.draft()), ui.request('pimaxFovea')]);
    check('The Boost plan answers next to other commands and is reused for unchanged Boost settings',Array.isArray(planA.plan.processes) && planB.pimax.found);
    { ui.renderBoostPlan(boostPlan); const b = boostPlan.plan, rows = $('boostPlan').querySelectorAll('.vram-table tbody tr'), select = $('input-flightDisplayMode');
      check('What happens at launch lists each program to close with its approximate VRAM and the total',rows.length === b.freeVram.length && b.freeVram.length === ui.profile.freeVramApps.length && !!$('vramTotal') && $('boostPlan').textContent.includes('approx'));
      const reported = (b.monitor?.modes || []).map(m => m.width+'x'+m.height+'@'+m.refresh), own = ui.profile.flightDisplayWidth+'x'+ui.profile.flightDisplayHeight+'@'+ui.profile.flightDisplayRefresh;
      check('The flight monitor mode offers only modes the monitor reports, and shows current to flight mode',[...select.options].every(o => reported.includes(o.value) || o.value === own) && select.value === own && (!b.monitor || $('monitorPlan').textContent.includes('→')));
      check('The small DCS window shows the options.lua values it sets',$('boostPlan').textContent.includes('graphics.width = 1280') && $('boostPlan').textContent.includes('graphics.fullScreen = false')); }
    check('What happens at launch comes from the backend without changing the draft',Array.isArray(boostPlan.plan.processes) && Array.isArray(boostPlan.plan.notes) && /dcs\.log$/i.test(boostPlan.dcsLog) && ui.profile.cpuBoost);
    $('featureCheck-boost').click(); check('Unchecking a feature renames the profile',!ui.profile.cpuBoost && ui.profile.name === 'Pimax · Quad Views + DLSS 5 + Frame generation');
    for (const route of ['Pimax','SboysSteamVr']) {
      select(route,{quad:true,dlss:true,framegen:true});
      edit('neuralRendering',false); edit('foveatedDlss',true); edit('quadFocusScale',150); edit('quadSharpening',.456789123456); edit('quadEdgeBlend',.15); edit('flowPreset','Slow'); edit('bidirectionalFlow',true); edit('nvidiaFlowScale',100);
      // DLSS off would bring back the Pimax native provider picked earlier: this route test uses the bundled one.
      edit('quadViews','QuadViewsFoveated');
      check(route+' with Quad Views Foveated Super Resolution stays off and hidden, so DLSS is off',!ui.profile.foveatedDlss && !ui.profile.quadFocusAdapter && $('foveatedDlssControls').hidden && !$('featureCheck-dlss').checked && $('featureSummary-dlss').textContent === 'Off · original image');
      edit('fpsLimit','Custom'); edit('renderedFpsCap',47.5); edit('disableDcsVSync',true);
      check(route+' custom quality retained',ui.profile.quadSharpening === .456789123456 && ui.profile.flowPreset === 'Slow');
      check(route+' the focus area says it comes from Pimax Play\'s settings',ui.profile.foveaSource === 'PimaxPlay' && $('featureSummary-quad').textContent.includes("from Pimax Play's settings") && $('input-foveaSource').querySelector('[value=PimaxPlay]').textContent === "From Pimax Play's settings");
      check(route+' before the first launch the status says changes apply at launch',$('statusTitle').textContent === 'Changes apply at launch' && !$('launch').disabled && !$('apply').disabled && $('openRecovery').hidden);
      await ui.run('preview'); check(route+' Review files lists every file read-only, with nothing to approve',ui.plan.files.length > 5 && !('token' in ui.plan) && !$('page-diagnostics').hidden && $('plannedFiles').children.length === ui.plan.files.length && !ui.state.launchReady && ui.state.originals.count === 0 && ui.state.status.includes('Launch DCS backs up the originals'));
      check(route+' Quad Views without DLSS 5 deploys no Cheeky file',!ui.plan.files.some(f => /CheekyFoveatedDLSS/i.test(f.path)));
      check(route+' review reads Pimax Play and reports it in Pimax Play units',ui.state.status.includes(ui.state.pimax.summary) && ui.state.pimax.summary.startsWith('Pimax Play · '+window.offlineFixture.pimaxMode+': Center Resolution 125%, Peripheral Resolution 20%') && ui.state.report.includes('Focus area from Pimax Play') && ui.plan.foveaStamp === ui.state.pimax.stamp);
      check(route+' exact FPS and VSync values reach native deployment',ui.plan.files.some(f => f.luaChanges?.some(c => c.path === 'graphics.maxFPS' && c.installedRaw === '47.5') && f.luaChanges.some(c => c.path === 'graphics.sync' && c.installedRaw === 'false')) && ui.state.cadence.dcsCap === 47.5);
      edit('renderedFpsCap',48); check(route+' an edit drops the reviewed file list',!ui.plan && $('fileCount').textContent === 'NOT REVIEWED'); edit('renderedFpsCap',47.5);
      // Launch DCS: one click applies with a backup and checks the launch contract; the offline host never starts DCS.
      $('launch').click(); await waitIdle();
      const firstApplied = ui.state.appliedProfile, firstAppliedName = ui.state.appliedName, firstCount = ui.state.originals.count;
      check(route+' Launch DCS backs up the originals, applies and verifies the launch contract without starting DCS',firstApplied && ui.state.launchReady && firstCount > 5 && ui.state.status.includes('DCS was not started') && ui.state.report.includes('Launch contract verified') && ui.state.report.includes('Profile: '+firstApplied));
      ui.navigate('recovery');
      check(route+' Recovery shows one Original files card with every changed file and one Back to stock DCS button',$('originals').querySelectorAll('.originals-card').length === 1 && $('originals').textContent.includes(firstCount+' files changed by DCS Control') && $('originals').querySelectorAll('.original-file').length === firstCount && !!$('restoreOriginals') && !!$('openBackups') && !$('originals').querySelector('[data-restore]'));
      ui.navigate('overview');
      check(route+' the applied draft reads Ready to fly, with Back to stock DCS one click away',ui.matchesApplied() && $('statusTitle').textContent === 'Ready to fly' && $('statusDot').classList.contains('ready') && $('apply').disabled && !$('launch').disabled && !$('openRecovery').hidden && !$('openRecovery').disabled);
      check(route+' Overview says Ready to fly',$('dashStatus').classList.contains('ready') && $('dashStatus').textContent.includes('Ready to fly') && $('resetApplied').hidden);
      edit('quadSharpening',.33); check(route+' an edited draft offers Undo changes',!$('resetApplied').hidden && $('dashStatus').textContent.includes('Changes apply at launch') && !!$('dashStatus').querySelector('[data-reset-applied]'));
      $('resetApplied').click(); check(route+' Undo changes brings back the applied profile',ui.matchesApplied() && $('resetApplied').hidden && $('statusTitle').textContent === 'Ready to fly');
      { const appliedOptions = ui.state.appliedOptions, appliedDcs = ui.state.appliedDcs;
        check(route+' the applied DCS install and options.lua are known to the page',Boolean(appliedOptions && appliedDcs));
        edit('options',appliedOptions.toUpperCase().replace(/\\/g,'/')); check(route+' the same options.lua spelled differently is still Ready to fly',ui.matchesApplied());
        edit('options',appliedOptions.replace(/options\.lua$/i,'other\\options.lua'));
        check(route+' another options.lua is a change, not Ready to fly',!ui.matchesApplied() && $('statusTitle').textContent === 'Changes apply at launch' && !$('resetApplied').hidden);
        $('resetApplied').click(); check(route+' Undo changes brings back the applied options.lua',ui.matchesApplied() && $('input-options').value === appliedOptions && $('statusTitle').textContent === 'Ready to fly'); }
      const appliedCount = ui.state.originals.count, appliedAt = ui.state.originals.lastActionAt; $('launch').click(); await waitIdle();
      check(route+' Launch with no changes writes nothing and launches the applied profile',ui.state.originals.count === appliedCount && ui.state.originals.lastActionAt === appliedAt && ui.state.appliedProfile === firstApplied && ui.state.status.startsWith('Offline verification') && ui.state.report.startsWith('The applied profile is up to date.'));
      edit('quadSharpening',.44); await ui.run('refresh');
      check(route+' a changed draft says changes apply at launch after a refresh',!ui.matchesApplied() && $('statusTitle').textContent === 'Changes apply at launch' && $('launch').title.startsWith('Apply ') && $('launch').title.includes(' over '+firstAppliedName));
      $('launch').click(); $('launch').click(); await waitIdle();
      const secondAt = ui.state.originals.lastActionAt;
      check(route+' Launch after a change writes the draft over the applied profile, with no restore step, and checks the launch contract',secondAt !== appliedAt && ui.state.report.includes(' over '+firstAppliedName) && !ui.state.report.includes('Restored') && ui.state.status.includes('DCS was not started') && ui.matchesApplied());
      check(route+' a double click launches once and the originals stay the first ones',ui.state.originals.count === appliedCount && !$('launch').disabled);
      edit('quadSharpening',.45); check(route+' the draft differs again, so Apply without launching is offered',!$('apply').disabled && $('statusTitle').textContent === 'Changes apply at launch');
      $('apply').click(); await waitIdle();
      check(route+' Apply without launching applies the draft and starts nothing',ui.matchesApplied() && ui.state.originals.lastActionAt !== secondAt && ui.state.status.includes('Launch DCS so it receives') && !ui.state.status.includes('not started'));
      await ui.request('launchCheck'); check(route+' saved launch contract verified',true);
      // DCS running (reported by the 4 s poll): Launch, Apply and Restore lock.
      ui.state.dcsRunning = true; ui.refreshWorkflow();
      check(route+' while DCS runs Launch, Apply and Restore are locked',$('statusTitle').textContent === 'DCS is running' && $('launch').disabled && $('apply').disabled && $('openRecovery').disabled && $('restoreOriginals').disabled);
      ui.state.dcsRunning = false; ui.refreshWorkflow(); check(route+' Launch unlocks when DCS closes',!$('launch').disabled && $('statusTitle').textContent === 'Ready to fly');
      $('openRecovery').click(); check('Back to stock DCS opens Recovery',!$('page-recovery').hidden);
      $('restoreOriginals').click(); await waitIdle(); check(route+' Back to stock DCS recovered every original file',!ui.state.launchReady && ui.state.originals.count === 0 && $('openRecovery').hidden && $('originals').textContent.includes('No changes to put back'));
    }
    edit('neuralRendering',true); edit('neuralIntensity',.42); edit('neuralLocalTone',1.23456789123);
    check('Active neural controls enabled',!$('input-neuralLocalTone').disabled);
    check('DLSS 5 runtime file sits on the DLSS 5 page and a missing file is flagged',ui.fields.find(f => f.path === 'neuralRuntimePath').page === 'dlss' && (ui.profile.neuralRuntimePath || $('dlssMissing').textContent.includes('nvngx_dlssnr.dll')));
    let failure = false; try { await ui.run('preview'); } catch(e) { failure = /nvngx_dlssnr/.test(e.message); }
    check('Missing neural DLL reported by Review files',failure && $('statusTitle').textContent === 'Needs attention');
    $('launch').click(); await waitIdle();
    check('Launch DCS with a missing neural DLL stops before writing anything and says why',ui.state.originals.count === 0 && !ui.state.launchReady && $('statusTitle').textContent === 'Needs attention' && /nvngx_dlssnr/.test($('status').textContent));
    $('statusDot').classList.remove('error'); $('toast').hidden = true;
    // Only a file that passes the deployment checks is kept as the app's runtime copy.
    let copyRefused = false; try { await ui.saveRuntime(window.offlineFixture.invalidRuntimePath); } catch(e) { copyRefused = /NVIDIA signature|310\.8/.test(e.message); }
    check('A runtime file without a valid NVIDIA signature is not saved',copyRefused && !ui.state.savedRuntime && $('savedRuntime').hidden && $('featureNotes-dlss').textContent.includes('Select file'));
    await ui.run('resetNeural'); check('Advanced reset preserves intensity and pipeline',ui.profile.neuralLocalTone === 1 && ui.profile.neuralIntensity === .42 && ui.profile.neuralRendering);
    if (window.offlineFixture.neuralRuntimePath) {
      for (const route of ['Pimax','SboysSteamVr']) {
        select(route,{quad:true,dlss:true,framegen:true});
        edit('neuralRendering',true); edit('neuralRuntimePath',window.offlineFixture.neuralRuntimePath);
        edit('neuralLocalTone',1.12); edit('neuralColorStrength',.87); edit('quadFocusScale',150); edit('flowPreset','Slow');
        edit('fpsLimit','MatchRefresh'); edit('headsetRefreshHz',90);
        await ui.run('preview'); check(route+' supplied neural DLL verified through web bridge',ui.plan.files.some(f => f.path.endsWith('nvngx_dlssnr.dll')));
        check(route+' combined neural pipeline carries half-refresh cap',ui.state.cadence.dcsCap === 45 && ui.plan.files.some(f => f.luaChanges?.some(c => c.path === 'graphics.maxFPS' && c.installedRaw === '45')));
        await ui.run('launch'); check(route+' neural + framegen + foveation deployed from WebView2',ui.state.launchReady && ui.matchesApplied());
        await ui.request('launchCheck'); check(route+' neural launch contract checked without DCS',true);
        await ui.run('restore'); check(route+' neural deployment restored',!ui.state.launchReady && ui.state.originals.count === 0);
      }
      // Selected once: the app keeps a verified copy, and a profile without a runtime path uses it.
      const kept = await ui.saveRuntime(window.offlineFixture.neuralRuntimePath);
      check('A selected neural DLL is verified and kept as the app copy',kept.savedRuntime?.version?.includes('310.8') && /[\\/]runtimes[\\/]nvngx_dlssnr\.dll$/i.test(kept.savedRuntime.path));
      select('Pimax',{quad:true,dlss:true,framegen:true}); edit('neuralRuntimePath','');
      check('With a saved copy DLSS 5 no longer asks for the file and shows where the copy lives',!ui.profile.neuralRuntimePath && !$('featureNotes-dlss').textContent.includes('Select file') && !$('featureSummary-dlss').textContent.includes('missing') && !$('dlssMissing').textContent.includes('nvngx_dlssnr') && $('savedRuntime').textContent.includes('Saved copy · version 310.8') && $('savedRuntime').textContent.includes(kept.savedRuntime.path) && $('savedRuntime').textContent.includes(window.offlineFixture.neuralRuntimePath) && !!$('forgetRuntime') && !!$('changeRuntime'));
      await ui.run('preview'); check('Preview deploys the saved copy for a profile without a runtime path',ui.plan.files.some(f => f.path.endsWith('nvngx_dlssnr.dll')));
      await ui.forgetRuntime(); check('Forget deletes the saved copy and the file is asked for again',!ui.state.savedRuntime && $('savedRuntime').hidden && $('featureNotes-dlss').textContent.includes('Select file') && !ui.plan);
    }
    for(const field of ui.fields.filter(f => f.type === 'number')) {
      edit(field.path,field.values[0]); check(field.path+' minimum accepted',!ui.invalid.has(field.path));
      edit(field.path,field.values[1]); check(field.path+' maximum accepted',!ui.invalid.has(field.path));
      edit(field.path,field.values[1]+1); check(field.path+' invalid entry rejected',ui.invalid.has(field.path));
      // Back to a value in range; a whole-number field (0 to 500 ms) takes a whole number.
      const back = field.path === 'neuralPaperWhiteScale' ? 1 : field.path.includes('Motion') ? 1 : Math.max(field.values[0],Math.min(field.values[1],.55));
      edit(field.path,Number.isInteger(field.values[2]) ? Math.round(back) : back);
    }
    // Values with a rule across fields: daylight at least 4 times the full-night level, pupil 0 (automatic) or 2 to 8 mm.
    edit('nightVisionScotopic',.005); edit('nightVisionPhotopic',5); edit('gazeDofPupilMm',0);
    edit('foveaWidth',.55); edit('foveaHeight',.45); edit('quadFocusScale',150); edit('quadSharpening',.7); edit('quadEdgeBlend',.2); edit('neuralWorkingScale',.75); edit('neuralIntensity',.35);
    edit('headsetRefreshHz',90); edit('renderedFpsCap',45); edit('fpsLimit','MatchRefresh');
    edit('foveatedDlss',false); // Foveated Super Resolution is a stereo-only DLSS-page option, independent of the checklist.
    // The DLSS row covers DLSS 5 and, in stereo, Foveated Super Resolution alone; Pimax native comes back once DLSS 5 no longer forces the bundled provider.
    select('Pimax',{...ui.featureState(ui.profile),quad:true,dlss:false}); edit('quadViews','PimaxNative');
    $('featureCheck-dlss').click(); check('DLSS on moves Pimax native to bundled Quad Views',ui.profile.quadViews === 'QuadViewsFoveated' && ui.profile.neuralRendering);
    $('featureCheck-dlss').click(); check('Pimax native comes back when DLSS is turned off',ui.profile.quadViews === 'PimaxNative' && !ui.profile.neuralRendering && !ui.profile.foveatedDlss);
    edit('foveatedDlss',true);
    check('With Quad Views Foveated Super Resolution is hidden and does not turn on',!ui.profile.foveatedDlss && $('foveatedDlssControls').hidden && !$('featureCheck-dlss').checked && ui.profile.quadViews === 'PimaxNative' && !$('input-quadViews').querySelector('[value=PimaxNative]').disabled);
    $('featureCheck-quad').click(); $('featureCheck-quad').click(); check('Quad Views turned back on uses the provider last selected',ui.profile.quadViews === 'PimaxNative');
    // Stereo Foveated Super Resolution: shown for stereo, and Cheeky writes ratios of at least 0.2, and the page says so.
    $('featureCheck-quad').click(); edit('foveatedDlss',true);
    check('Stereo Foveated Super Resolution alone checks the DLSS row and lights the DLSS tile',!$('foveatedDlssControls').hidden && $('setting-foveatedDlss').textContent.includes('For stereo without Quad Views') && $('featureCheck-dlss').checked && $('featureSummary-dlss').textContent.startsWith('Foveated Super Resolution on') && $('pipeline').querySelector('[data-page=dlss]').classList.contains('on') && !$('input-quadViews').querySelector('[value=PimaxNative]').disabled);
    $('featureCheck-dlss').click(); check('Unchecking the DLSS row turns DLSS 5 and Foveated Super Resolution off',!ui.profile.foveatedDlss && !ui.profile.neuralRendering && ui.profile.quadViews === 'None');
    edit('foveatedDlss',true); edit('peripheralScale',.15);
    check('Stereo Foveated Super Resolution shows the 0.2 Cheeky writes',$('help-peripheralScale').textContent.includes('0.2 is written') && ui.effectiveFovea(ui.profile).periphery === .2 && !$('help-foveaWidth').textContent.includes('is written'));
    edit('peripheralScale',.55); $('featureCheck-quad').click();
    check('Turning Quad Views on turns Foveated Super Resolution off and hides it',ui.profile.quadViews === 'PimaxNative' && !ui.profile.foveatedDlss && !$('featureCheck-dlss').checked && $('foveatedDlssControls').hidden);
    edit('quadViews','QuadViewsFoveated'); edit('neuralRendering',true);
    const wizard = window.setupWizard, beforeWizard = JSON.stringify(ui.profile);
    const pick = (id,value) => { $(id).value = value; $(id).dispatchEvent(new Event('change')); };
    $('guidedSetup').click(); check('Guided setup opens with a three-step stepper',$('setupDialog').open && $('setupSteps').children.length === 3);
    $('setupClose').click(); check('Cancel setup preserves the exact draft',JSON.stringify(ui.profile) === beforeWizard);
    wizard.open(); check('Guided setup chooses with the same feature checkboxes',$('setupFeatures').querySelectorAll('input[type=checkbox]').length === 5);
    pick('setupRoute','SboysSteamVr'); wizard.choose({features:{quad:true,dlss:false,framegen:false,boost:false}}); pick('setupGaze','EyeTracked');
    await wizard.next();
    check('Wizard blocks missing registered driver',wizard.step === 1 && $('setupNext').disabled && wizard.report.checks.some(c => c.id === 'sboys-registration' && c.state === 'Error'));
    check('Wizard lists problems first and folds passed checks',$('setupChecks').querySelectorAll('.readiness-item.Pass').length === 0 && !!$('setupBody').querySelector('.setup-passed'));
    check('Wizard probe does not overwrite the current quality draft',JSON.stringify(ui.profile) === beforeWizard);
    $('setupClose').click(); wizard.open();
    pick('setupRoute','Pimax'); wizard.choose({features:{quad:false,dlss:false,framegen:false,boost:false}});
    check('Focus movement is hidden for a pipeline without Quad Views',$('setupGazeRow').hidden);
    await wizard.next(); const originalPath = $('setupPath-dcs').value; $('setupPath-dcs').value += '.missing'; $('setupPath-dcs').dispatchEvent(new Event('input')); await wizard.check();
    check('Wizard rejects a missing game path and opens the file paths',!wizard.report.canPrepare && $('setupNext').disabled && $('setupBody').querySelector('.setup-files').open);
    $('setupPath-dcs').value = originalPath; $('setupPath-dcs').dispatchEvent(new Event('input')); await wizard.check();
    check('Wizard verifies the baseline files without claiming headset validation',wizard.report.canPrepare && wizard.report.checks.some(c => c.state === 'Manual'));
    check('Wizard retains precise custom quality and pacing',wizard.candidate.quadFocusScale === ui.profile.quadFocusScale && wizard.candidate.neuralLocalTone === ui.profile.neuralLocalTone && wizard.candidate.neuralWorkingScale === ui.profile.neuralWorkingScale && wizard.candidate.fpsLimit === 'MatchRefresh' && wizard.candidate.headsetRefreshHz === 90);
    await wizard.next(); check('Wizard ends on the same single Launch DCS and writes nothing before it',wizard.step === 2 && !ui.state.launchReady && $('setupNext').textContent.startsWith('Launch DCS') && ui.state.originals.count === 0);
    const wizardCandidate = JSON.stringify(wizard.candidate);
    await wizard.next(); check('Wizard Launch DCS applies the checked profile and closes',!$('setupDialog').open && ui.state.launchReady && ui.matchesApplied() && ui.profile.id === JSON.parse(wizardCandidate).id && ui.state.status.includes('DCS was not started'));
    // A second launch replaces the applied profile: the new draft is written over it, no restore step in between.
    const firstApplied = ui.state.appliedName, firstAt = ui.state.originals.lastActionAt;
    edit('frameGen','Nvidia'); $('launch').click(); await waitIdle();
    check('Launch switches profiles by writing over the applied one',ui.state.launchReady && ui.state.originals.lastActionAt !== firstAt && ui.state.report.includes(' over '+firstApplied) && ui.matchesApplied());
    wizard.open(); pick('setupRoute','Pimax'); wizard.choose({features:{quad:false,dlss:false,framegen:false,boost:false}}); await wizard.next();
    check('Wizard accepts an applied profile as replaceable',wizard.report.canPrepare && wizard.report.checks.some(c => c.id === 'active-backup' && c.state === 'Pass') && $('setupNext').textContent === 'Continue →' && $('setupBody').textContent.includes('Launch DCS writes the new one over it'));
    { const beforeKeep = JSON.stringify(ui.profile); await wizard.next(); $('setupUseDraft').click();
      check('Wizard can keep its choice as the draft without launching',!$('setupDialog').open && JSON.stringify(ui.profile) !== beforeKeep && !ui.profile.frameGen.startsWith('Nvidia') && !ui.matchesApplied() && ui.state.launchReady); }
    await ui.run('restore'); check('Wizard deployments restored',!ui.state.launchReady && ui.state.originals.count === 0);
    await ui.run('preview'); edit('frameGen','Nvidia'); check('Editing after the wizard drops the reviewed file list',!ui.plan && $('fileCount').textContent === 'NOT REVIEWED');
    // The DCS launcher: refused, and said so, only when this app runs inside a job that forbids breakaway.
    check('The launcher option reports whether the DCS launcher can start the game here',typeof ui.state.launcherBlocked === 'boolean' && typeof ui.state.launcherUnknown === 'boolean' && (!ui.state.launcherBlocked || ($('setting-keepDcsLauncher').textContent.includes('error 5') && (ui.profile.keepDcsLauncher || $('input-keepDcsLauncher').disabled))));
    if (ui.state.launcherBlocked) { ui.profile.keepDcsLauncher = true; ui.invalidate(); await ui.run('refresh'); check('With the launcher kept, Checks says why it cannot start the game',ui.checkItems().some(c => c.id === 'dcs-launcher' && c.group === 'fix')); ui.profile.keepDcsLauncher = false; ui.invalidate(); await ui.run('refresh'); }
    else { ui.profile.keepDcsLauncher = true; ui.invalidate(); await ui.run('refresh'); check('With the launcher kept and no job in the way, nothing blocks it',ui.profile.keepDcsLauncher && !ui.checkItems().some(c => c.id === 'dcs-launcher' && c.group === 'fix')); ui.profile.keepDcsLauncher = false; ui.invalidate(); await ui.run('refresh'); }
    let offlineDownloadRejected = false; try { await ui.run('prepareSboys'); } catch(e) { offlineDownloadRejected = /Network downloads are disabled/.test(e.message); } check('Unbundled driver download is blocked in offline fixtures',offlineDownloadRejected);
    // Final screenshots show a complete configurable route, without an applied game modification.
    edit('quadViews','QuadViewsFoveated'); edit('foveatedDlss',true);
    check('With Quad Views Foveated Super Resolution never turns on, and the focus adapter follows DLSS 5',!ui.profile.foveatedDlss && ui.profile.quadFocusAdapter === Boolean(ui.profile.neuralRendering) && !$('featureSummary-dlss').textContent.includes('Foveated'));
    await ui.run('refresh'); $('toast').hidden = true;
    ui.navigate('overview'); check('No fixture change remains',ui.state.originals.count === 0 && !ui.state.launchReady);
    window.chrome.webview.postMessage({smokeResult:{passed:true,count:checks.length,checks,mode:'Hidden HWND; real WebView2 + real .NET bridge; isolated deployment fixtures; no DCS launch'}});
  } catch(error) { window.chrome.webview.postMessage({smokeResult:{passed:false,count:checks.length,checks,error:error.stack}}); }
};

/* Browser regression checks for collapsed states, keyboard input and shader lifecycle. */
async (page) => {
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', entry => { if (entry.type() === 'error') errors.push(entry.text()); });
  await page.emulateMedia({reducedMotion: 'reduce'});
  await page.setViewportSize({width: 1440, height: 900});
  await page.reload();
  await page.evaluate(() => document.fonts.ready);
  const check = (ok, label) => { if (!ok) throw new Error(label); };
  const settle = async () => page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  await settle();
  const query = page.getByRole('combobox'), anchor = await query.boundingBox();
  const collapsed = await page.locator('.launcher').boundingBox();
  check(await page.locator('#clear').count() === 0, 'Clear icon remains');
  const variants = {};
  for (const variant of [0,1,2]) {
    await page.locator(`button[data-art-variant="${variant}"]`).click(); await settle();
    variants[variant] = await page.evaluate(() => ({variant:searchArt.variant,program:searchArt.gl.getParameter(searchArt.gl.CURRENT_PROGRAM)!==null,error:searchArt.gl.getError()}));
    check(variants[variant].variant===variant && variants[variant].program && variants[variant].error===0,'Artwork selection failed');
    check((await query.boundingBox()).y===anchor.y,'Artwork selection moved query');
    await page.locator('.launcher').screenshot({path:`docs/designs/quiet-system/art-study-${variant}.png`});
  }
  await page.locator('button[data-art-variant="0"]').click(); await settle();
  check(await query.inputValue() === '', 'Preview did not start empty');
  check(collapsed.height <= 120, 'Empty popup has unused space');
  for (const selector of ['#results', '#message', '.launcher-footer']) check(await page.locator(selector).isHidden(), `Idle reveals ${selector}`);
  await page.screenshot({path:'docs/designs/quiet-system/preview.png'});
  await page.locator('.launcher').screenshot({path:'docs/designs/quiet-system/launcher.png'});
  const states = {};
  for (const state of ['results', 'none', 'offline', 'idle']) {
    await page.locator(`button[data-state="${state}"]`).click(); await settle();
    states[state] = {query: await query.boundingBox(), popup: await page.locator('.launcher').boundingBox()};
    check(states[state].query.x === anchor.x && states[state].query.y === anchor.y && states[state].query.width === anchor.width, `Query moved in ${state}`);
    if (state !== 'results') {
      check(await page.locator('.launcher-footer').isHidden(), `Footer appears in ${state}`);
      check(states[state].popup.height === collapsed.height, `Non-result state expanded in ${state}`);
    }
    if (state === 'results') {
      check(await page.getByRole('option').count() === 5, 'Missing fixture results');
      await page.locator('.launcher').screenshot({path:'docs/designs/quiet-system/results.png'});
    }
    if (state === 'offline') {
      check(await query.inputValue() === 'config', 'Offline lost query');
      await query.press('Enter'); await settle();
      check(await page.getByRole('option').count() === 5, 'Keyboard Retry failed');
    }
  }
  await query.fill('config'); await settle();
  await query.press('ArrowDown');
  check(await query.getAttribute('aria-activedescendant') === 'result-1', 'Arrow selection failed');
  await query.press('Control+Enter');
  check((await page.locator('#action-note').textContent()).includes('reveal config.c'), 'Reveal action failed');
  await query.press('Home'); await settle();
  check(await page.locator('#underline-caret').evaluate(n => !n.hidden && parseFloat(n.style.left) === 0), 'Home caret failed');
  await query.press('Shift+End'); await settle();
  check(await page.locator('#underline-caret').evaluate(n => n.hidden), 'Selection caret remains visible');
  await query.press('Control+A'); await query.press('Backspace'); await settle();
  check(await query.inputValue() === '', 'Keyboard clearing failed');
  check((await page.locator('.launcher').boundingBox()).height === collapsed.height, 'Clearing failed to collapse');
  await query.fill('   '); await settle();
  check(await page.locator('#results').isHidden() && await page.locator('#message').isHidden(), 'Whitespace showed a panel');
  const mobile = {};
  for (const width of [390, 320]) {
    await page.setViewportSize({width, height:900});
    await page.locator('button[data-state="results"]').click(); await settle();
    mobile[width] = await page.locator('.result-path').evaluateAll(nodes => nodes.map(n => ({text:n.textContent, full:n.title, fits:n.scrollWidth<=n.clientWidth})));
    check(mobile[width].every(path => path.fits), `Path overflow at ${width}`);
    check(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `Horizontal overflow at ${width}`);
    await page.locator('button[data-state="idle"]').click(); await settle();
    check((await page.locator('.launcher').boundingBox()).height <= 120, `Narrow idle expanded at ${width}`);
  }
  check(mobile[320][1].text === '~/…/torchlight/src/core', 'Final folders lost');
  await page.screenshot({path:'output/playwright/quiet-art-narrow.png'});
  await page.setViewportSize({width:1440,height:900});
  // Check input-driven feedback independently of result/opening animations.
  await page.emulateMedia({reducedMotion:'no-preference'});
  await page.locator('button[data-state="idle"]').click();
  check(await page.locator('.search-surface').evaluate(n => !n.classList.contains('is-typing')), 'State control lit border');
  await query.fill('co');
  await page.waitForTimeout(400);
  await query.press('n');
  await page.waitForTimeout(400);
  check(await page.locator('.search-surface').evaluate(n => n.classList.contains('is-typing')), 'Continuous input failed to extend light');
  await query.fill('config'); await page.waitForTimeout(150);
  const typing = await page.locator('.search-surface').evaluate(n => ({opacity:getComputedStyle(n,'::after').opacity, border:getComputedStyle(n,'::after').borderColor}));
  check(Number(typing.opacity) === 1, 'Typing edge failed to brighten');
  const litQuery = await query.boundingBox();
  check(litQuery.x === anchor.x && litQuery.y === anchor.y && litQuery.width === anchor.width, 'Typing light moved query');
  await page.locator('.launcher').screenshot({path:'docs/designs/quiet-system/typing.png'});
  await page.waitForFunction(() => !document.querySelector('.search-surface').classList.contains('is-typing'));
  await page.waitForFunction(() => Number(getComputedStyle(document.querySelector('.search-surface'),'::after').opacity) === 0);
  await query.press('ArrowDown');
  check(await page.locator('.search-surface').evaluate(n => !n.classList.contains('is-typing')), 'Navigation lit border');
  await query.fill('config.h'); await query.blur();
  check(await page.evaluate(() => typingTimer === null && !searchSurface.classList.contains('is-typing')), 'Blur retained typing timer');
  await query.focus(); await query.fill('config'); await query.press('Escape');
  check(await page.evaluate(() => typingTimer === null && !searchSurface.classList.contains('is-typing')), 'Dismiss retained typing timer');
  await page.locator('button[data-state="idle"]').click();
  await page.emulateMedia({reducedMotion:'reduce'}); await query.fill('config'); await settle();
  check(await page.locator('.search-surface').evaluate(n => getComputedStyle(n,'::after').transitionDuration === '0s' && getComputedStyle(n,'::after').opacity === '1'), 'Reduced motion animated typing light');
  await query.blur(); await settle();
  check(await page.locator('.search-surface').evaluate(n => getComputedStyle(n,'::after').opacity === '0'), 'Reduced motion failed to reset border');
  await query.focus();
  await page.emulateMedia({reducedMotion:'no-preference'}); await settle();
  const before = await page.evaluate(() => searchArt.time);
  await page.waitForTimeout(160);
  check(await page.evaluate(() => searchArt.time) > before, 'Shader time did not advance');
  await query.press('Escape');
  check(await page.evaluate(() => searchArt.frame === null), 'Dismissed shader is still scheduled');
  const paused = await page.evaluate(() => searchArt.time);
  await page.waitForTimeout(100);
  check(await page.evaluate(() => searchArt.time) === paused, 'Dismissed shader advanced');
  await page.locator('button[data-state="idle"]').click(); await page.waitForTimeout(100);
  check(await page.evaluate(() => searchArt.time) > paused, 'Shader failed to resume');
  await page.emulateMedia({reducedMotion:'reduce'}); await settle();
  check(await page.evaluate(() => searchArt.frame === null && document.getAnimations().length === 0), 'Reduced motion left active animation');
  const reducedTime = await page.evaluate(() => searchArt.time);
  await page.waitForTimeout(100);
  check(await page.evaluate(() => searchArt.time) === reducedTime, 'Reduced shader still moves');
  const context = await page.evaluate(() => {
    const gl = searchArt.canvas.getContext('webgl'), extension = gl?.getExtension('WEBGL_lose_context');
    if (!extension) return false;
    window.__artLoss = extension;
    extension.loseContext(); return true;
  });
  check(context, 'Missing context loss test extension');
  await page.waitForFunction(() => document.querySelector('.search-surface').dataset.art === 'fallback');
  check(await query.isVisible(), 'Context loss hid query');
  await page.evaluate(() => { window.__artLoss.restoreContext(); delete window.__artLoss; });
  await page.waitForFunction(() => document.querySelector('.search-surface').dataset.art === 'webgl');
  await settle();
  check(await page.evaluate(() => searchArt.canvas.getContext('webgl').getError() === 0), 'Context recovery GL error');
  await page.emulateMedia({reducedMotion:'no-preference'});
  await page.waitForTimeout(60);
  check(await page.evaluate(() => searchArt.frame !== null), 'Visibility test did not start an animation');
  await page.evaluate(() => {
    Object.defineProperty(document, 'hidden', {configurable:true, value:true});
    document.dispatchEvent(new Event('visibilitychange'));
  });
  check(await page.evaluate(() => searchArt.frame === null), 'Hidden document retained animation');
  const hiddenTime = await page.evaluate(() => searchArt.time);
  await page.waitForTimeout(80);
  check(await page.evaluate(() => searchArt.time) === hiddenTime, 'Hidden document advanced shader time');
  await page.evaluate(() => { delete document.hidden; document.dispatchEvent(new Event('visibilitychange')); });
  check(await page.evaluate(() => searchArt.frame !== null), 'Visible document did not resume shader');
  await page.emulateMedia({reducedMotion:'reduce'});
  check(errors.length === 0, errors.join('; '));
  return {checks:'passed', collapsed, states, variants, mobile, typingLight:{...typing, continuousInput:true, stationary:true, quietAfterIdle:true, navigationQuiet:true, timerCleanup:true, reducedMotion:true, clearIconRemoved:true, keyboardClear:true}, contextRecovery:context, browserErrors:errors};
}

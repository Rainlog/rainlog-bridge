// Browser integration test with fixture HTTP responses. Run from repo root.
// Requires Playwright and Chromium; PLAYWRIGHT_MODULE/CHROMIUM_PATH can override them.
const http = require("http"),
  fs = require("fs"),
  assert = require("assert");
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || "playwright");
(async () => {
  let manualUploads = 0, rejectManual = true;
  const firmware = Buffer.alloc(1024);
  firmware[0] = 0xe9; firmware[23] = 1;
  firmware.writeUInt32LE(0xabcd5432, 32);
  firmware.write('1.0.0', 48);
  firmware.write('rainlog-wireless-bridge', 80);
  firmware.write('RLOGOTA1', 288);
  firmware.write('esp32-c6fh8-lcd-1.47', 296);
  let wifiActive = true;
  let radioMappings = [];
  let wuMappings = [];
  let radioName = "";
  let wifiGauge = 12345;
  let radioPage = true,
    saved;
  const server = http.createServer((req, res) => {
    res.setHeader("Content-Type", "application/json");
    if (req.url === "/config")
      return res.end(
        JSON.stringify({
          sta_ssid: "Home network",
          ap_ssid: "RainlogBridge",
          ap_pass_default: false,
          ap_ip: "10.41.0.1",
          wu_map: wuMappings,
          radio_enabled: true,
          wifi_interception_enabled: false,
          wifi_interception_active: radioPage ? wifiActive : true,
          radio_map: radioMappings,
        }),
      );
    if (req.url === "/radio")
      return res.end(
        JSON.stringify({
          available: true,
          receiving: true,
          error: 0,
          sensors: [
            { model: 0, sensor_id: 4, channel: 0, gauge_id: 0, age_s: 2, rssi_dbm: -82.5, name: radioName },
            { model: 1, sensor_id: 3271, channel: 67, gauge_id: 0, age_s: 8 },
          ],
        }),
      );
    if (req.url === "/scan") return res.end("[]");
    if (req.url === "/clients")
      return res.end(
        JSON.stringify([
          {
            mac: "01:02:03:04:05:06",
            name: "",
            hostname: "Weather console",
            vendor: "AcuRite",
            ip: "10.41.0.2",
            connected: true,
            rssi: -50,
            age_s: 1,
            you: false,
            rx: 1,
            rl: 1,
            wu: 0,
            err: 0,
            gauge_id: wifiGauge,
          },
        ]),
      );
    if (req.url === "/ota/status")
      return res.end(
        JSON.stringify({
          board: "esp32-c6fh8-lcd-1.47",
          max_image_size: 0x300000,
          phase: "uptodate",
          running: "1.0.0",
          latest: "1.0.0",
          available: false,
          progress: 0,
          error: "",
        }),
      );
    if (req.url === '/ota/upload') {
      assert.equal(req.headers['x-rainlog-ota'], '1');
      assert.equal(req.headers['content-type'], 'application/octet-stream');
      const chunks = [];
      req.on('data', data => chunks.push(data));
      return req.on('end', () => {
        assert.deepEqual(Buffer.concat(chunks), firmware);
        manualUploads++;
        if (rejectManual) {
          res.statusCode = 400;
          res.end(JSON.stringify({error: 'Image verification failed; current firmware retained'}));
        } else res.end(JSON.stringify({ok: true, rebooting: true}));
      });
    }
    if (req.url === '/rename') {
      let body = '';
      req.on('data', chunk => body += chunk);
      return req.on('end', () => {
        const form = new URLSearchParams(body);
        assert.equal(form.get('mac'), 'radio:0:4:0');
        radioName = form.get('name');
        res.end('{}');
      });
    }
    if (req.url === "/save") {
      let body = "";
      req.on("data", (s) => (body += s));
      return req.on("end", () => {
        saved = new URLSearchParams(body);
        res.end("{}");
      });
    }
    res.setHeader("Content-Type", "text/html");
    res.setHeader("Content-Encoding", "gzip");
    res.end(
      fs.readFileSync(
        "main/web/" + (radioPage ? "radio433/" : "") + "index.html.gz",
      ),
    );
  });
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const browser = await chromium.launch({
    executablePath: process.env.CHROMIUM_PATH || "/usr/bin/chromium",
    headless: true,
    args: ["--no-sandbox"],
  });
  const page = await browser.newPage({
    viewport: { width: 430, height: 1000 },
  });
  let errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  const url = "http://127.0.0.1:" + server.address().port;
  await page.goto(url + '/firmware');
  assert.equal(await page.locator('#viewFirmware').isVisible(), true);
  await page.locator('#hardwareTag').getByText('Hardware tag: esp32-c6fh8-lcd-1.47', {exact:true}).waitFor();
  await page.getByRole('link', {name: 'Devices', exact: true}).click();
  assert.equal(new URL(page.url()).pathname, '/devices');
  await page.goBack();
  assert.equal(await page.locator('#viewFirmware').isVisible(), true);
  await page.goForward();
  assert.equal(await page.locator('#viewDevices').isVisible(), true);
  await page.reload();
  assert.equal(await page.locator('#viewDevices').isVisible(), true);
  await page.getByRole('link', {name: 'Setup', exact: true}).click();
  assert.equal(new URL(page.url()).pathname, '/setup');
  await page.waitForFunction(() => document.querySelector('[name=ap_ssid]').value !== '');
  assert.equal(await page.locator('[name=ap_ssid]').isDisabled(), true);
  assert.equal(await page.locator('[name=ap_pass]').isDisabled(), true);
  assert.equal(await page.locator('#radioEnabled').isDisabled(), true);
  await page.locator('#wifiInterceptionEnabled').check();
  await page.locator('#radioEnabled').uncheck();
  assert.equal(await page.locator('#wifiInterceptionEnabled').isDisabled(), true);
  await page.locator('#radioEnabled').check();
  assert.equal(await page.locator('[name=ap_ssid]').isEnabled(), true);
  assert.equal(await page.locator('[name=ap_pass]').isEnabled(), true);
  await page.locator('#wifiInterceptionEnabled').uncheck();
  assert.equal(await page.locator('#radioSetup button').count(), 0);
  assert.equal(await page.locator('#radioSetup #radioRows').count(), 0);
  await page.locator('#radioStatus').getByText('Radio reception enabled. 2 devices seen.', {exact: true}).waitFor();
  await page.getByRole("link", { name: "Devices", exact: true }).click();
  await page
    .locator("#radioDeviceList")
    .getByText("La Crosse TX5U", { exact: false })
    .waitFor();
  await page.getByText("Weather console", { exact: true }).waitFor();
  const wifiCard = page.locator('.wifi-card');
  assert.match(await wifiCard.locator('.wifi-rainlog').textContent(), /Rainlog12345/);
  assert.equal(await wifiCard.locator('.wifi-rainlog h4').textContent(), 'Rainlog Uploader Enabled');
  assert.equal(await wifiCard.locator('.wifi-rainlog .uploader-stats').textContent(), '1 received · 1 to Rainlog');
  assert.equal(await wifiCard.locator('.device-stats').textContent(), '');
  assert.equal(await wifiCard.locator('.wifi-rainlog input').count(), 0);
  assert.equal(await wifiCard.locator('.radio-signal i.active').count(), 4);
  assert.equal(await wifiCard.evaluate(card => {
    const panel = card.querySelector('.wifi-rainlog');
    const button = card.querySelector('.wu-uploader-action');
    return !!(panel.compareDocumentPosition(button) & Node.DOCUMENT_POSITION_FOLLOWING)
      && button.getBoundingClientRect().top >= panel.getBoundingClientRect().bottom;
  }), true);
  await wifiCard.getByRole('button', {name: 'Add WU Uploader', exact: true}).click();
  assert.equal(await wifiCard.locator('.wurow .rl').inputValue(), 'Rainlog12345');
  assert.equal(await page.locator('#radioDeviceList .radio-card').count(), 2);
  assert.equal(await page.locator('.radio-card').filter({hasText:'AcuRite Iris'}).evaluate(card => {
    const rainlog = card.querySelector('.mapping-action').getBoundingClientRect();
    const wu = card.querySelector('.wu-uploader-action').getBoundingClientRect();
    return Math.abs(rainlog.top - wu.top) < 1 && wu.left >= rainlog.right
      && Math.abs(rainlog.width - wu.width) < 1 && Math.abs(rainlog.height - wu.height) < 1;
  }), true);

  const signal = page.locator('.radio-card').filter({hasText: 'La Crosse TX5U'}).locator('.radio-signal');
  assert.equal(await signal.isVisible(), true);
  assert.match(await signal.textContent(), /-82.5 dBm/);
  assert.equal(await signal.locator('i.active').count(), 2);
  assert.equal(await page.getByRole('button', {name:'Rename', exact:true}).count(), 0);
  page.once('dialog', dialog => dialog.accept('Back yard bucket'));
  await page.locator('.radio-card').filter({hasText:'La Crosse TX5U'}).getByRole('button', {name:'Edit device name', exact:true}).click();
  await page.locator('.radio-card .device-name h4').filter({hasText:'Back yard bucket'}).waitFor();
  await Promise.all([
    page.waitForResponse(response => response.url().endsWith('/radio')),
    page.evaluate(() => document.dispatchEvent(new Event('visibilitychange'))),
  ]);
  assert.equal(await page.locator('.radio-card').filter({hasText:'La Crosse TX5U'}).locator('h4').first().textContent(), 'Back yard bucket');


  await page.locator('.radio-card').filter({hasText: 'La Crosse TX5U'}).getByRole('button', {name: 'Add Rainlog Uploader', exact: true}).click();
  assert.equal(await page.locator('[data-field=id]').inputValue(), '4');
  assert.equal(await page.locator('[data-field=id]').getAttribute('type'), 'hidden');
  assert.equal(await page.locator('[data-field=model]').getAttribute('type'), 'hidden');
  assert.equal(await page.locator('[data-field=channel]').getAttribute('type'), 'hidden');
  assert.equal(await page.locator('.radio-card').filter({hasText: 'La Crosse TX5U'}).locator('[data-field=gauge]').count(), 1);
  assert.equal(await page.locator("[data-field=gauge]").inputValue(), "");
  await page.locator("[data-field=gauge]").fill("Rainlog12345");
  assert.match(await page.locator('[data-field=gauge]').evaluate(input => input.validationMessage), /Wi-Fi device/);
  await page.locator("[data-field=gauge]").fill("Rainlog12346");
  assert.equal(await page.locator('[data-field=gauge]').evaluate(input => input.validationMessage), '');
  const radioCard = page.locator('.radio-card').filter({hasText: 'La Crosse TX5U'});
  assert.equal(await radioCard.getByRole('button', {name: 'Add WU Uploader', exact: true}).isEnabled(), true);
  await radioCard.getByRole('button', {name: 'Add WU Uploader', exact: true}).click();
  await radioCard.locator('.wurow .wu').fill('KRADIO');
  await radioCard.locator('.wurow .wk').fill('radio-wu-key');

  await page.locator("[data-field=key]").fill("test-pws-key");
  assert.equal(await page.getByRole('button', {name: 'Replace sensor', exact: true}).count(), 0);
  await Promise.all([
    page.waitForResponse(response => response.url().endsWith('/radio')),
    page.evaluate(() => document.dispatchEvent(new Event('visibilitychange'))),
  ]);
  assert.equal(await page.locator('[data-field=gauge]').inputValue(), 'Rainlog12346');
  assert.equal(await page.locator('[data-field=key]').inputValue(), 'test-pws-key');
  assert.equal(await radioCard.locator('.wurow .wu').inputValue(), 'KRADIO');
  await page.getByRole("link", { name: "Devices", exact: true }).click();
  await page.locator(".wifi-card .wurow .wu").fill("KTESTSTATION");
  await page.locator(".wifi-card .wurow .wk").fill("test-wu-key");
  assert.equal(await page.locator(".wurow .stationPicker").count(), 0);
  assert.equal(await page.locator(".wurow .rl").first().getAttribute("type"), "hidden");
  assert.equal(await page.locator(".wifi-card .wurow .rl").inputValue(), "Rainlog12345");

  const iris = page.locator('.radio-card').filter({hasText:'AcuRite Iris'});
  await iris.getByRole('button', {name:'Add WU Uploader', exact:true}).click();
  await iris.locator('.wu').fill('KIRIS');
  await iris.locator('.wk').fill('iris-wu-key');
  assert.equal(await iris.locator('.radio-row').count(), 0);
  await page.screenshot({
    path: "/tmp/rainlog-radio-web-mobile.png",
    fullPage: true,
  });
  await page.getByRole("button", { name: "Save & reboot bridge" }).click();
  await page.waitForURL("**/save");
  assert.equal(saved.get("wd0"), "01:02:03:04:05:06");
  assert.equal(saved.get("wd1"), "radio:0:4:0");
  assert.equal(saved.get("wd2"), "radio:1:3271:67");
  assert.equal(saved.get("rl2"), "");
  assert.equal(saved.get("wu2"), "KIRIS");
  assert.equal(saved.get("rl0"), "Rainlog12345");
  assert.equal(saved.get("wu0"), "KTESTSTATION");
  assert.equal(saved.get("wk0"), "test-wu-key");
  assert.equal(saved.get("rl1"), "Rainlog12346");
  assert.equal(saved.get("wu1"), "KRADIO");
  assert.equal(saved.get("wk1"), "radio-wu-key");
  assert.equal(saved.get("radio_id0"), "4");
  assert.equal(saved.get("radio_gauge0"), "Rainlog12346");
  assert.equal(saved.get("radio_key0"), "test-pws-key");
  assert.equal(saved.get("radio_enabled"), "on");
  assert.equal(saved.get("wifi_interception_enabled"), null);
  wuMappings = [{gauge_id:54321, wu_id:'KSAVED', wu_key:'saved-key'}];
  radioMappings = [
    {model: 0, sensor_id: 9, channel: 0, gauge_id: 54321, rainlog_key: 'keep-tx5u-key'},
    {model: 1, sensor_id: 3271, channel: 67, gauge_id: 54322, rainlog_key: 'keep-iris-key'},
  ];
  await page.goto(url);
  await page.getByRole("link", {name: 'Devices', exact: true}).click();
  await page.getByText(/This TX5U has not been seen/).waitFor();
  assert.equal(await page.getByRole('button', {name: 'Replace sensor', exact: true}).count(), 1);
  const txCard = page.locator('.radio-card').filter({has: page.locator('[data-field=model][value="0"]')});
  const txPicker = txCard.locator('.sensor-picker');
  await txCard.locator('.wurow .wu').waitFor();
  assert.equal(await txCard.locator('.wurow .wu').inputValue(), 'KSAVED');
  assert.equal(await txPicker.isDisabled(), true);
  await page.getByRole('button', {name: 'Replace sensor', exact: true}).click();
  assert.equal(await txPicker.locator('option[value="1:3271:67"]').count(), 0);
  await txPicker.selectOption('0:4:0');
  assert.equal(await txPicker.isDisabled(), true);
  assert.equal(await page.getByRole('button', {name: 'Replace sensor', exact: true}).count(), 0);
  assert.equal(await txCard.locator('[data-field=gauge]').inputValue(), 'Rainlog54321');
  assert.equal(await txCard.locator('[data-field=key]').inputValue(), 'keep-tx5u-key');
  assert.equal(await txCard.locator('.wurow .wd').inputValue(), 'radio:0:4:0');
  await page.getByRole('button', {name: 'Save & reboot bridge', exact: true}).click();
  await page.waitForURL('**/save');
  const txIndex = Array.from(saved.keys()).find(key => /^radio_id[0-9]+$/.test(key) && saved.get(key) === '4').slice('radio_id'.length);
  assert.equal(saved.get('radio_gauge' + txIndex), 'Rainlog54321');
  assert.equal(saved.get('radio_key' + txIndex), 'keep-tx5u-key');
  radioMappings = [];
  wuMappings = [];
  wifiActive = false;
  await page.goto(url);
  await page.getByRole("link", { name: "Devices", exact: true }).click();
  await page
    .locator("#radioDeviceList")
    .getByText("La Crosse TX5U", { exact: false })
    .waitFor();
  assert.equal(await page.locator("#wifiDevices").isVisible(), false);
  assert.equal(await page.getByText('Other WU Uploaders', {exact: true}).count(), 0);
  assert.equal(await page.locator('#addBtn').count(), 0);
  radioPage = false;
  wifiGauge = 0;
  wuMappings = [{device:'01:02:03:04:05:06', gauge_id:0, wu_id:'KWIFIONLY', wu_key:'wifi-only-key'}];
  await page.goto(url);
  await page.getByRole('link', {name:'Devices', exact:true}).click();
  await page.locator('.wifi-card .wurow .wu').waitFor();
  assert.equal(await page.locator('.wifi-card .wurow .wu').inputValue(), 'KWIFIONLY');
  assert.equal(await page.locator('.wifi-card .wifi-rainlog').count(), 0);
  await page.locator('.wifi-card .wurow').getByRole('button', {name:'Remove WU Uploader', exact:true}).click();
  assert.equal(await page.locator('.wifi-card').getByRole('button', {name:'Add WU Uploader', exact:true}).isEnabled(), true);

  assert.equal(await page.locator("#radioSetup").count(), 0);
  assert.equal(await page.locator("#wifiInterceptionEnabled").count(), 0);
  await page.getByRole("link", {name: 'Firmware', exact: true}).click();
  await page.locator('#manualUpdate summary').click();
  await page.locator('#firmwareFile').setInputFiles({name: 'short.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(20)});
  assert.equal(await page.locator('#firmwareInstall').isDisabled(), true);
  const wrongBoard = Buffer.from(firmware);
  wrongBoard.fill(0, 296, 336); wrongBoard.write('lilygo-t3-v1.6.1-sx1278', 296);
  await page.locator('#firmwareFile').setInputFiles({name: 'wrong-board.bin', mimeType: 'application/octet-stream', buffer: wrongBoard});
  await page.getByText('This firmware is for a different board.', {exact: true}).waitFor();
  assert.equal(await page.locator('#firmwareInstall').isDisabled(), true);
  assert.equal(manualUploads, 0);
  await page.locator('#firmwareFile').setInputFiles({name: 'app.bin', mimeType: 'application/octet-stream', buffer: firmware});
  await page.getByText(/Version 1.0.0 · esp32-c6fh8/).waitFor();
  await page.getByRole('button', {name: 'Upload & install', exact: true}).click();
  await page.getByText('Image verification failed; current firmware retained', {exact: true}).waitFor();
  assert.equal(await page.locator('#firmwareInstall').isEnabled(), true);
  rejectManual = false;
  await page.getByRole('button', {name: 'Upload & install', exact: true}).click();
  await page.getByText('Firmware verified. Rebooting; reconnect to the bridge shortly.', {exact: true}).waitFor();
  assert.equal(manualUploads, 2);
  assert.deepEqual(errors, []);
  console.log(
    "Device cards, name editing, device-keyed WU uploaders, legacy attachment, WU-only sensors, combined submission and radio-disabled page passed",
  );
  await browser.close();
  server.close();
})().catch((e) => {
  console.error(e);
  process.exit(1);
});

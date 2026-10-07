// Browser integration test with fixture HTTP responses. Run from repo root.
// Requires Playwright and Chromium; PLAYWRIGHT_MODULE/CHROMIUM_PATH can override them.
const http = require("http"),
  fs = require("fs"),
  assert = require("assert");
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || "playwright");
(async () => {
  let wifiActive = true;
  let radioMappings = [];
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
          wu_map: [],
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
            { model: 0, sensor_id: 4, channel: 0, gauge_id: 0, age_s: 2 },
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
            gauge_id: 12345,
          },
        ]),
      );
    if (req.url === "/ota/status")
      return res.end(
        JSON.stringify({
          phase: "uptodate",
          running: "1.0.0",
          latest: "1.0.0",
          available: false,
          progress: 0,
          error: "",
        }),
      );
    if (req.url === "/save") {
      let body = "";
      req.on("data", (s) => (body += s));
      return req.on("end", () => {
        saved = new URLSearchParams(body);
        res.end("{}");
      });
    }
    res.setHeader("Content-Type", "text/html");
    res.end(
      fs.readFileSync(
        "main/web/" + (radioPage ? "radio433/" : "") + "index.html",
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
  await page.goto(url);
  await page.waitForFunction(() => document.querySelector('[name=ap_ssid]').value !== '');
  assert.equal(await page.locator('[name=ap_ssid]').isDisabled(), true);
  assert.equal(await page.locator('[name=ap_pass]').isDisabled(), true);
  await page.locator('#wifiInterceptionEnabled').check();
  assert.equal(await page.locator('[name=ap_ssid]').isEnabled(), true);
  assert.equal(await page.locator('[name=ap_pass]').isEnabled(), true);
  await page.locator('#wifiInterceptionEnabled').uncheck();
  assert.equal(await page.locator('#radioSetup button').count(), 0);
  assert.equal(await page.locator('#radioSetup #radioRows').count(), 0);
  await page.locator('#radioStatus').getByText('Radio reception enabled. 2 devices seen.', {exact: true}).waitFor();
  await page.getByRole("button", { name: "Devices", exact: true }).click();
  await page
    .locator("#radioDeviceList")
    .getByText("La Crosse TX5U", { exact: false })
    .waitFor();
  await page.getByText("Weather console", { exact: true }).waitFor();
  await page.waitForFunction(
    () => document.querySelector("#wuRows .rl").value === "Rainlog12345",
  );
  assert.equal(
    await page
      .locator("#wuRows .stationPicker option[value=Rainlog12345]")
      .getAttribute("value"),
    "Rainlog12345",
  );
  assert.equal(await page.locator('#radioDeviceList .radio-card').count(), 2);

  await page.locator('.radio-card').filter({hasText: 'La Crosse TX5U'}).getByRole('button', {name: 'Add mapping', exact: true}).click();
  assert.equal(await page.locator('[data-field=id]').inputValue(), '4');
  assert.equal(await page.locator('[data-field=id]').getAttribute('type'), 'hidden');
  assert.equal(await page.locator('[data-field=model]').getAttribute('type'), 'hidden');
  assert.equal(await page.locator('[data-field=channel]').getAttribute('type'), 'hidden');
  assert.equal(await page.locator('.radio-card').filter({hasText: 'La Crosse TX5U'}).locator('[data-field=gauge]').count(), 1);
  assert.equal(await page.locator("[data-field=gauge]").inputValue(), "");
  await page.locator("[data-field=gauge]").fill("Rainlog12345");
  await page.locator("[data-field=key]").fill("test-pws-key");
  assert.equal(await page.getByRole('button', {name: 'Replace sensor', exact: true}).count(), 0);
  await Promise.all([
    page.waitForResponse(response => response.url().endsWith('/radio')),
    page.evaluate(() => document.dispatchEvent(new Event('visibilitychange'))),
  ]);
  assert.equal(await page.locator('[data-field=gauge]').inputValue(), 'Rainlog12345');
  assert.equal(await page.locator('[data-field=key]').inputValue(), 'test-pws-key');
  await page.getByRole("button", { name: "Devices", exact: true }).click();
  await page.locator("#wuRows .wu").fill("KTESTSTATION");
  await page.locator("#wuRows .wk").fill("test-wu-key");
  await page.locator("#wuRows .stationPicker").selectOption("Rainlog12345");
  assert.equal(await page.locator("#wuRows .rl").inputValue(), "Rainlog12345");
  await page.locator("#wuRows .rl").fill("Rainlog98765");
  assert.equal(await page.locator("#wuRows .rl").inputValue(), "Rainlog98765");
  await page.screenshot({
    path: "/tmp/rainlog-radio-web-mobile.png",
    fullPage: true,
  });
  await page.getByRole("button", { name: "Save & reboot bridge" }).click();
  await page.waitForURL("**/save");
  assert.equal(saved.get("rl0"), "Rainlog98765");
  assert.equal(saved.get("wu0"), "KTESTSTATION");
  assert.equal(saved.get("wk0"), "test-wu-key");
  assert.equal(saved.get("radio_id0"), "4");
  assert.equal(saved.get("radio_gauge0"), "Rainlog12345");
  assert.equal(saved.get("radio_key0"), "test-pws-key");
  assert.equal(saved.get("radio_enabled"), "on");
  assert.equal(saved.get("wifi_interception_enabled"), null);
  radioMappings = [
    {model: 0, sensor_id: 9, channel: 0, gauge_id: 54321, rainlog_key: 'keep-tx5u-key'},
    {model: 1, sensor_id: 3271, channel: 67, gauge_id: 54322, rainlog_key: 'keep-iris-key'},
  ];
  await page.goto(url);
  await page.getByRole('button', {name: 'Devices', exact: true}).click();
  await page.getByText(/This TX5U has not been seen/).waitFor();
  assert.equal(await page.getByRole('button', {name: 'Replace sensor', exact: true}).count(), 1);
  const txCard = page.locator('.radio-card').filter({has: page.locator('[data-field=model][value="0"]')});
  const txPicker = txCard.locator('.sensor-picker');
  assert.equal(await txPicker.isDisabled(), true);
  await page.getByRole('button', {name: 'Replace sensor', exact: true}).click();
  assert.equal(await txPicker.locator('option[value="1:3271:67"]').count(), 0);
  await txPicker.selectOption('0:4:0');
  assert.equal(await txPicker.isDisabled(), true);
  assert.equal(await page.getByRole('button', {name: 'Replace sensor', exact: true}).count(), 0);
  assert.equal(await txCard.locator('[data-field=gauge]').inputValue(), 'Rainlog54321');
  assert.equal(await txCard.locator('[data-field=key]').inputValue(), 'keep-tx5u-key');
  await page.getByRole('button', {name: 'Save & reboot bridge', exact: true}).click();
  await page.waitForURL('**/save');
  const txIndex = Array.from(saved.keys()).find(key => /^radio_id[0-9]+$/.test(key) && saved.get(key) === '4').slice('radio_id'.length);
  assert.equal(saved.get('radio_gauge' + txIndex), 'Rainlog54321');
  assert.equal(saved.get('radio_key' + txIndex), 'keep-tx5u-key');
  radioMappings = [];
  wifiActive = false;
  await page.goto(url);
  await page.getByRole("button", { name: "Devices", exact: true }).click();
  await page
    .locator("#radioDeviceList")
    .getByText("La Crosse TX5U", { exact: false })
    .waitFor();
  assert.equal(await page.locator("#wifiDevices").isVisible(), false);
  radioPage = false;
  await page.goto(url);
  assert.equal(await page.locator("#radioSetup").count(), 0);
  assert.deepEqual(errors, []);
  console.log(
    "Devices lists, WU station suggestions/manual entry, combined form submission and radio-disabled page passed",
  );
  await browser.close();
  server.close();
})().catch((e) => {
  console.error(e);
  process.exit(1);
});

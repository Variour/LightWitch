import { access, mkdir } from 'node:fs/promises';
import { constants as fsConstants } from 'node:fs';
import path from 'node:path';
import { chromium } from 'playwright';

const DEFAULT_BASE_URL = process.env.UI_BASE_URL || 'http://127.0.0.1:8080';
const CANDIDATE_EXECUTABLES = [
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  '/opt/pw-browsers/chromium',
].filter(Boolean);

async function firstExistingPath(paths) {
  for (const candidate of paths) {
    try {
      await access(candidate, fsConstants.X_OK);
      return candidate;
    } catch {
      // Try the next candidate.
    }
  }
  return undefined;
}

export async function launchUi(options = {}) {
  const {
    baseUrl = DEFAULT_BASE_URL,
    headless = true,
    launchOptions = {},
    contextOptions = {},
    gotoOptions = { waitUntil: 'networkidle' },
  } = options;

  const executablePath = await firstExistingPath(CANDIDATE_EXECUTABLES);
  const browser = await chromium.launch({
    headless,
    ...(executablePath ? { executablePath } : {}),
    ...launchOptions,
  });
  const context = await browser.newContext(contextOptions);
  const page = await context.newPage();
  await page.goto(baseUrl, gotoOptions);
  return { browser, context, page, baseUrl };
}

export async function saveScreenshot(page, name, options = {}) {
  const dir = options.dir || path.join('test-results', 'ui');
  await mkdir(dir, { recursive: true });
  const fileName = name.endsWith('.png') ? name : `${name}.png`;
  const filePath = path.join(dir, fileName);
  await page.screenshot({ path: filePath, fullPage: true, ...options.pageOptions });
  return filePath;
}

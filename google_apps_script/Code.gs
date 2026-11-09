/**
 * AQUASENSE test logger  -  paste this into  Extensions > Apps Script  of a Google Sheet.
 * Every test sent by the ESP32 becomes ONE ROW in the "Tests" sheet.
 * Re-sent tests (same Test ID) are ignored, so no duplicates.
 */
const SECRET_TOKEN = 'CHANGE_ME_LONG_RANDOM_TOKEN';  // must equal GOOGLE_TOKEN in include/secrets.h
const SHEET_NAME   = 'Tests';
const TIMEZONE     = 'Asia/Manila';

const HEADERS = [
  'Test ID', 'Date & Time', 'Time source', 'Device',
  'pH', 'TDS (ppm)', 'Turbidity (NTU)', 'Temperature (°C)',
  'DO est. (mg/L)', 'EC est. (µS/cm)',
  'Passed (of 6)', 'Verdict', 'Failed parameters', 'Sample / Notes'
];
// bit order used by the firmware: bit0..bit5
const PARAM_NAMES = ['pH', 'Temperature', 'TDS', 'Turbidity', 'EC', 'DO'];

function getSheet_() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sh = ss.getSheetByName(SHEET_NAME);
  if (!sh) sh = ss.insertSheet(SHEET_NAME);
  if (sh.getLastRow() === 0) {
    sh.appendRow(HEADERS);
    sh.setFrozenRows(1);
    sh.getRange(1, 1, 1, HEADERS.length).setFontWeight('bold').setBackground('#0F7C86').setFontColor('#FFFFFF');
    sh.setColumnWidths(1, HEADERS.length, 130);
  }
  return sh;
}

function out_(text) {
  return ContentService.createTextOutput(text).setMimeType(ContentService.MimeType.TEXT);
}

function doPost(e) {
  const lock = LockService.getScriptLock();
  lock.waitLock(20000);
  try {
    const d = JSON.parse(e.postData.contents);
    if (d.token !== SECRET_TOKEN) return out_('ERROR: bad token');

    const sh  = getSheet_();
    const key = String(d.device) + '-' + String(d.id);

    // duplicate protection (the ESP32 may retry if it never saw our answer)
    const last = sh.getLastRow();
    if (last > 1) {
      const ids = sh.getRange(2, 1, last - 1, 1).getValues();
      for (let i = 0; i < ids.length; i++) {
        if (String(ids[i][0]) === key) return out_('OK duplicate');
      }
    }

    const deviceTime = d.ts && d.ts > 1700000000;
    const when = deviceTime ? new Date(d.ts * 1000) : new Date();
    const whenText = Utilities.formatDate(when, TIMEZONE, 'yyyy-MM-dd HH:mm:ss');
    const timeSrc  = deviceTime ? 'Device clock (NTP)' : 'Upload time (device had no clock)';

    const failed = [];
    for (let b = 0; b < 6; b++) if (!((d.mask >> b) & 1)) failed.push(PARAM_NAMES[b]);
    const blank = v => (v === null || v === undefined) ? '' : v;

    sh.appendRow([
      key, whenText, timeSrc, d.device,
      d.ph, d.tds, d.turb, blank(d.temp), blank(d.do), d.ec,
      d.passed, d.safe ? 'SAFE' : 'UNSAFE', failed.length ? failed.join(', ') : 'None', ''
    ]);
    return out_('OK');
  } catch (err) {
    return out_('ERROR: ' + err);
  } finally {
    lock.releaseLock();
  }
}

// Opening the web app URL in a browser just shows this line (handy for testing).
function doGet() {
  return out_('AQUASENSE logger is running');
}

import { get, put, del } from '@vercel/blob';
import { GoogleGenAI } from '@google/genai';

const BLOB_PREFIX = 'calmfit-state';

function blobPath(key) { return `${BLOB_PREFIX}/${key}.json`; }

async function blobRead(key) {
  try {
    const result = await get(blobPath(key), { access: 'private', useCache: false });
    if (!result) return null;
    const text = await new Response(result.stream).text();
    return JSON.parse(text);
  } catch (error) {
    if (error?.statusCode === 404 || error?.code === 'not_found') return null;
    throw error;
  }
}

async function blobWrite(key, value) {
  await put(blobPath(key), JSON.stringify(value), { access: 'private', addRandomSuffix: false, allowOverwrite: true });
}

async function blobDelete(key) {
  try { await del(blobPath(key)); } catch {}
}
const DEVICE_TOKEN = process.env.CALMFIT_DEVICE_TOKEN || '';
const ACCESS_TOKEN = process.env.CALMFIT_ACCESS_TOKEN || '';
const SAMPLE_INTERVAL_MS = 10_000;
const DEVICE_TIMEOUT_MS = 15_000;
// 높은 단계가 감지되면 최소 이 시간 동안 유지 (대시보드 1.5초 폴링 사이에 지나간 과부하를 놓치지 않도록)
const STAGE_HOLD_MS = 5_000;
const MAX_SAMPLES_PER_DAY = 5000;

// GY-61 감도를 높인 단계 전환 기준
const MOTION_NOISE_FLOOR = 2;
const MOTION_CAUTION_THRESHOLD = 8;
const MOTION_DANGER_THRESHOLD = 22;
const DEFAULT_ELEVATED_HR = 98;
const DEFAULT_HIGH_HR = 115;
const DAY_KEYS = Array.from({ length: 14 }, (_, i) => `day-${i + 1}`);

const STANDARD_BASE_GUIDE = {
  source: 'standard_baseline',
  isCustom: false,
  statusMessage: '14일 실제 수집 전 표준 지원 가이드. 14일 실제 데이터가 모두 완료되면 개인화 AI 가이드가 활성화됩니다.',
  stages: {
    0: { stageName: '0단계: 안정 (Stable)', badge: '안정 상태', summary: '현재 측정된 심박수와 움직임이 기준보다 크게 올라가지 않은 상태입니다.', actions: [
      { title: '현재 루틴 유지', desc: '갑작스러운 일정 변화 없이 평소 편안한 환경을 유지합니다.', tag: '환경' },
      { title: '조용히 관찰', desc: '아이가 편안하게 하고 있는 활동을 방해하지 않고 상태를 지켜봅니다.', tag: '관찰' },
      { title: '평소 사용하는 지원 방법 유지', desc: '보호자가 이미 사용하던 편안한 방법이 있다면 평소 방식대로 제공합니다.', tag: '지원' },
    ] },
    1: { stageName: '1단계: 주의 (Caution)', badge: '주의', summary: '심박수 또는 움직임 변화가 관찰되고 있습니다. 자극을 줄이고 반응을 천천히 살펴봅니다.', actions: [
      { title: '주변 자극 줄이기', desc: '불필요한 소리와 화면, 복잡한 활동을 줄여봅니다.', tag: '감각 조절' },
      { title: '선호하는 공간으로 이동 고려', desc: '아이가 평소 편안해하는 조용한 공간이 있다면 이동을 고려합니다.', tag: '공간' },
      { title: '말을 짧게 하기', desc: '짧고 차분한 문장으로 안내하고 여러 질문을 한꺼번에 하지 않습니다.', tag: '소통' },
    ] },
    2: { stageName: '2단계: 경고 (Warning)', badge: '주의 깊은 지원 필요', summary: '심박수와 움직임 중 강한 변화가 관찰되거나 여러 신호가 함께 나타나고 있습니다.', actions: [
      { title: '자극을 더 줄이기', desc: '사람이 많은 곳이나 불필요한 소리 등 추가 자극을 가능한 범위에서 줄입니다.', tag: '감각 조절' },
      { title: '익숙한 지원 도구 확인', desc: '아이가 평소 받아들였던 감각 도구나 편안한 활동을 보호자 판단으로 제공합니다.', tag: '지원 도구' },
      { title: '안전한 거리에서 차분히 대기', desc: '상태를 관찰하며 필요한 안내만 짧게 제공합니다.', tag: '관찰' },
    ] },
    3: { stageName: '3단계: 매우 위험 (Danger)', badge: '안전 우선', summary: '현재 센서 신호가 강하게 변한 상태입니다. 안전 확보와 차분한 관찰을 우선합니다.', actions: [
      { title: '주변 안전 확인', desc: '걸려 넘어지거나 부딪힐 위험이 있는 물건을 가능한 범위에서 치웁니다.', tag: '안전' },
      { title: '자극 최소화', desc: '불필요한 말과 소리를 줄이고 안전한 공간을 확보합니다.', tag: '환경' },
      { title: '필요 시 주변 성인에게 도움 요청', desc: '혼자 대응하기 어렵거나 신체적 안전이 우려되면 주변의 책임 있는 성인에게 도움을 요청합니다.', tag: '도움 요청' },
    ] },
  },
  generatedAt: Date.now(),
};

function json(data, status = 200) {
  return new Response(JSON.stringify(data), { status, headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' } });
}

function parseSafeNumber(value) {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && value.trim() !== '') { const n = Number(value); return Number.isFinite(n) ? n : null; }
  return null;
}

function parseBoolean(value) { return value === true || value === 1 || value === '1' || value === 'true'; }

function sameOrigin(req) {
  const origin = req.headers.get('origin');
  if (!origin) return false;
  try { return new URL(origin).origin === new URL(req.url).origin; } catch { return false; }
}

function accessAuthorized(req) {
  if (sameOrigin(req)) return true;
  if (!ACCESS_TOKEN) return false;
  return (req.headers.get('authorization') || '') === `Bearer ${ACCESS_TOKEN}`;
}

function deviceAuthorized(req) { return Boolean(DEVICE_TOKEN) && req.headers.get('x-calmfit-device-token') === DEVICE_TOKEN; }

async function readJson(key, fallback = null) { return (await blobRead(key)) ?? fallback; }
async function writeJson(key, value) { await blobWrite(key, value); }

function initialize14Days() {
  const result = [];
  const today = new Date();
  for (let i = 1; i <= 14; i++) {
    const d = new Date(today); d.setDate(today.getDate() - (14 - i));
    result.push({ dayNumber: i, dateStr: d.toISOString().split('T')[0], isCompleted: false, sampleCount: 0, avgHeartRate: 0, minHeartRate: 0, maxHeartRate: 0, avgMotionLevel: 0, overloadEventCount: 0,
      soundCounts: { calm_voice: 0, groaning: 0, crying: 0, screaming: 0, ambient_noise: 0 }, voiceLabelCounts: {}, samples: [] });
  }
  return result;
}

async function ensureDaysInitialized() {
  const initial = initialize14Days();
  const days = [];
  for (let i = 0; i < 14; i++) {
    const existing = await readJson(DAY_KEYS[i]);
    if (existing && existing.dayNumber === i + 1) days.push(existing);
    else { await writeJson(DAY_KEYS[i], initial[i]); days.push(initial[i]); }
  }
  return days;
}

async function loadDays() { return ensureDaysInitialized(); }

async function readMeta() {
  return (await readJson('meta', null)) || { latestTelemetry: null, lastSeenTimestamp: 0, lastSampleAt: 0, previousAccel: { x: 0, y: 0, z: 0, initialized: false }, stage: evaluateStage(0, 0, 'NORMAL'), personalizedBaseline: null, collectionLocked: false };
}

async function writeMeta(meta) { await writeJson('meta', meta); }
async function readGuide() { return await readJson('guide-current', null); }
async function writeGuide(guide) { await writeJson('guide-current', guide); }
async function readActionLogs() { return (await readJson('action-logs', [])) || []; }
async function writeActionLogs(logs) { await writeJson('action-logs', logs.slice(0, 100)); }

function evaluateStage(heartRate, motionScore, motionLevelStr, soundCategory, voiceLabel, childBaseline) {
  const baseline = childBaseline?.calmBpm;
  const elevatedThreshold = childBaseline?.elevatedThreshold || DEFAULT_ELEVATED_HR;
  const highThreshold = Math.max(DEFAULT_HIGH_HR, elevatedThreshold + 15);
  const validHr = typeof heartRate === 'number' && heartRate > 0 ? heartRate : 0;
  const score = Math.max(0, Math.min(100, Number.isFinite(motionScore) ? motionScore : 0));
  const reasons = [];
  const isMotionDanger = score >= MOTION_DANGER_THRESHOLD || motionLevelStr === 'BIG BIG MOVE';
  const isMotionElevated = score >= MOTION_CAUTION_THRESHOLD || motionLevelStr === 'BIG MOVE';
  const isSlightMotion = score >= MOTION_NOISE_FLOOR;
  let hrState = 'normal';
  if (validHr > 0) {
    if (validHr >= highThreshold) { hrState = 'high'; reasons.push('heartRate_high'); }
    else if (validHr >= elevatedThreshold) { hrState = 'elevated'; reasons.push('heartRate_above_baseline'); }
  }
  if (isMotionDanger) reasons.push('motion_severe_high'); else if (isMotionElevated) reasons.push('motion_increased'); else if (isSlightMotion) reasons.push('motion_slight_shake');
  const isStressVoice = ['groaning', 'crying', 'screaming'].includes(soundCategory) || (typeof voiceLabel === 'string' && voiceLabel.toUpperCase().includes('VOICE_3'));
  if (isStressVoice) reasons.push('stress_voice_detected');
  let stageCode = 0, stage = 'STABLE', stageName = '안정';
  if ((isMotionDanger && hrState === 'high') || (isMotionDanger && isStressVoice) || (hrState === 'high' && isStressVoice)) { stageCode = 3; stage = 'DANGER'; stageName = '매우 위험'; }
  else if (isMotionDanger || (isMotionElevated && hrState === 'elevated') || hrState === 'high' || (isMotionElevated && isStressVoice)) { stageCode = 2; stage = 'WARNING'; stageName = '경고 단계'; }
  else if (isMotionElevated || hrState === 'elevated' || isSlightMotion || isStressVoice) { stageCode = 1; stage = 'CAUTION'; stageName = '주의 단계'; }
  else reasons.push('normal_baseline_state');
  return { stage, stageCode, stageName, heartRate: validHr, ...(typeof baseline === 'number' ? { baselineHeartRate: baseline } : {}), motionScore: Math.round(score), reasons, evaluatedAt: Date.now() };
}

function calculateAnalytics(days) {
  const completed = days.filter(d => d.isCompleted && Array.isArray(d.samples) && d.samples.length > 0);
  const allSamples = days.flatMap(d => Array.isArray(d.samples) ? d.samples : []);
  const daysWithHeartRate = completed.filter(d => d.samples.some(s => typeof s.heartRate === 'number' && s.heartRate > 0));
  const is14DaysCompleted = completed.length >= 14 && allSamples.length >= 70 && daysWithHeartRate.length >= 14;
  if (!allSamples.length) return { is14DaysCompleted: false, completedDaysCount: completed.length, totalSamplesCount: 0, daysCollected: completed.length, daysRemaining: 14 - completed.length, daysWithHeartRate: daysWithHeartRate.length, message: '아직 분석에 필요한 실제 센서 데이터가 부족합니다.', realStats: null };
  const validHr = allSamples.filter(s => typeof s.heartRate === 'number' && s.heartRate > 0);
  const calm = validHr.filter(s => s.aiState === 0);
  const average = validHr.length ? Math.round(validHr.reduce((a, s) => a + s.heartRate, 0) / validHr.length) : 0;
  const min = validHr.length ? Math.min(...validHr.map(s => s.heartRate)) : 0;
  const max = validHr.length ? Math.max(...validHr.map(s => s.heartRate)) : 0;
  const calmAverage = calm.length ? Math.round(calm.reduce((a, s) => a + s.heartRate, 0) / calm.length) : (average || null);
  const motions = allSamples.map(s => Number(s.motionScore) || 0);
  const avgMotion = motions.length ? Math.round(motions.reduce((a, b) => a + b, 0) / motions.length) : 0;
  const maxMotion = motions.length ? Math.max(...motions) : 0;
  const voiceCounts = {};
  for (const s of allSamples) if (s.voiceLabel && s.voiceLabel !== 'unknown' && s.voiceLabel !== 'none') voiceCounts[s.voiceLabel] = (voiceCounts[s.voiceLabel] || 0) + 1;
  const elevatedThreshold = typeof calmAverage === 'number' ? Math.round(calmAverage * 1.25) : null;
  return { is14DaysCompleted, completedDaysCount: completed.length, totalSamplesCount: allSamples.length, daysCollected: completed.length, daysRemaining: Math.max(0, 14 - completed.length), daysWithHeartRate: daysWithHeartRate.length,
    message: is14DaysCompleted ? '14일 실제 데이터 수집 완료' : `${completed.length}/14일 수집 완료 (심박수 확인 ${daysWithHeartRate.length}/14일)`,
    realStats: { collectedDays: completed.length, totalSamples: allSamples.length, heartRate: { calmAverage, average, min, max, elevatedThreshold }, motion: { average: avgMotion, max: maxMotion }, voiceLabelCounts: voiceCounts } };
}

function motionFromAccel(x, y, z, previous) {
  if (!previous?.initialized) return { score: 0, next: { x, y, z, initialized: true } };
  const delta = Math.abs(x - previous.x) + Math.abs(y - previous.y) + Math.abs(z - previous.z);
  const score = Math.round(Math.max(0, Math.min(100, (delta / 300) * 100)));
  return { score, next: { x, y, z, initialized: true } };
}

function normalizeGuide(guide, analytics, currentStage) {
  if (!guide || typeof guide !== 'object') throw new Error('AI 응답이 객체가 아닙니다.');
  const normalized = JSON.parse(JSON.stringify(guide));
  normalized.source = 'ai_custom'; normalized.isCustom = true; normalized.generatedAt = Date.now();
  normalized.currentState = { stageCode: currentStage.stageCode, stageName: currentStage.stageName, reasons: currentStage.reasons, heartRate: currentStage.heartRate, motionScore: currentStage.motionScore };
  normalized.baseline = normalized.baseline || {};
  normalized.baseline.calmBpm = analytics.realStats.heartRate.calmAverage ?? null;
  normalized.baseline.elevatedThreshold = analytics.realStats.heartRate.elevatedThreshold ?? null;
  if (!Array.isArray(normalized.baseline.primarySensoryTriggers)) normalized.baseline.primarySensoryTriggers = [];
  normalized.stages = normalized.stages || {};
  for (const key of ['0', '1', '2', '3']) {
    normalized.stages[key] = normalized.stages[key] || { ...STANDARD_BASE_GUIDE.stages[key] };
    if (!Array.isArray(normalized.stages[key].actions)) normalized.stages[key].actions = STANDARD_BASE_GUIDE.stages[key].actions;
  }
  return normalized;
}

async function handleTelemetry(req) {
  if (!deviceAuthorized(req) && !sameOrigin(req)) return json({ ok: false, error: '기기 인증 토큰이 필요합니다.' }, 401);
  const payload = await req.json().catch(() => null);
  if (!payload || typeof payload !== 'object') return json({ ok: false, error: '유효한 JSON 본문이 필요합니다.' }, 400);
  const deviceId = typeof payload.deviceId === 'string' ? payload.deviceId.trim() : '';
  if (!/^[A-Za-z0-9_-]{3,64}$/.test(deviceId)) return json({ ok: false, error: '유효한 deviceId가 필요합니다.' }, 400);
  const meta = await readMeta();
  const hr = parseSafeNumber(payload.heartRate) ?? 0;
  const rawIrValue = parseSafeNumber(payload.rawIrValue) ?? 0;
  const accelX = parseSafeNumber(payload.accelX) ?? 0;
  const accelY = parseSafeNumber(payload.accelY) ?? 0;
  const accelZ = parseSafeNumber(payload.accelZ) ?? 0;
  const suppliedMotion = parseSafeNumber(payload.motionScore);
  const motionCalc = motionFromAccel(accelX, accelY, accelZ, meta.previousAccel);
  const motionScore = Math.round(suppliedMotion !== null ? suppliedMotion : motionCalc.score);
  const motionLevel = typeof payload.motionLevel === 'string' && payload.motionLevel.trim() ? payload.motionLevel.trim() : (motionScore >= MOTION_DANGER_THRESHOLD ? 'BIG BIG MOVE' : motionScore >= MOTION_CAUTION_THRESHOLD ? 'BIG MOVE' : 'NORMAL');
  const contact = payload.contact !== undefined ? parseBoolean(payload.contact) : (hr > 0 || rawIrValue > 5000);
  const soundCategory = typeof payload.soundCategory === 'string' ? payload.soundCategory : 'none';
  const voiceLabel = typeof payload.voiceLabel === 'string' && payload.voiceLabel.trim() ? payload.voiceLabel.trim() : undefined;
  const voiceConfidence = parseSafeNumber(payload.voiceConfidence);
  const now = Date.now();
  const nextStage = evaluateStage(hr, motionScore, motionLevel, soundCategory, voiceLabel, meta.personalizedBaseline || undefined);
  // 피크 단계 유지: 더 높은 단계가 최근 STAGE_HOLD_MS 안에 있었다면 그 단계를 유지
  const held = meta.heldStage;
  let effectiveStage;
  let heldStage;
  if (held && typeof held.stageCode === 'number' && held.stageCode > nextStage.stageCode && now < (held.heldUntil || 0)) {
    effectiveStage = { ...held, rawStageCode: nextStage.stageCode };
    heldStage = held;
  } else {
    heldStage = { ...nextStage, heldUntil: now + STAGE_HOLD_MS };
    effectiveStage = { ...nextStage, rawStageCode: nextStage.stageCode };
  }
  const telemetry = { deviceId, timestamp: typeof payload.timestamp === 'number' ? payload.timestamp : now, serverReceivedAt: now, heartRate: Math.round(hr), rawIrValue: Math.round(rawIrValue), motionScore, motionLevel,
    accelX: Math.round(accelX), accelY: Math.round(accelY), accelZ: Math.round(accelZ), contact, soundCategory, voiceLabel, voiceConfidence: voiceConfidence ?? undefined };
  let lastSampleAt = meta.lastSampleAt || 0;
  const shouldSample = contact && (hr > 0 || rawIrValue > 5000) && !meta.collectionLocked && (now - lastSampleAt >= SAMPLE_INTERVAL_MS);
  if (shouldSample) {
    const days = await loadDays();
    const activeDay = days.find(d => !d.isCompleted);
    if (activeDay && activeDay.samples.length < MAX_SAMPLES_PER_DAY) {
      const sample = { timestamp: now, dateStr: activeDay.dateStr, timeStr: new Date(now).toLocaleTimeString('ko-KR', { hour12: false }), heartRate: hr > 0 ? Math.round(hr) : 0,
        motionScore, motionLevel, soundCategory, voiceLabel, voiceConfidence: voiceConfidence ?? undefined, contact, aiState: nextStage.stageCode };
      activeDay.samples.push(sample); activeDay.sampleCount = activeDay.samples.length;
      const validHrs = activeDay.samples.filter(s => s.heartRate > 0).map(s => s.heartRate);
      if (validHrs.length) { activeDay.avgHeartRate = Math.round(validHrs.reduce((a, b) => a + b, 0) / validHrs.length); activeDay.minHeartRate = Math.min(...validHrs); activeDay.maxHeartRate = Math.max(...validHrs); }
      const validMotion = activeDay.samples.map(s => Number(s.motionScore) || 0); activeDay.avgMotionLevel = validMotion.length ? Number((validMotion.reduce((a, b) => a + b, 0) / validMotion.length).toFixed(1)) : 0;
      activeDay.overloadEventCount = activeDay.samples.filter(s => s.aiState >= 2).length;
      if (soundCategory in activeDay.soundCounts) activeDay.soundCounts[soundCategory] += 1;
      if (voiceLabel && voiceLabel !== 'unknown' && voiceLabel !== 'none') { activeDay.voiceLabelCounts = activeDay.voiceLabelCounts || {}; activeDay.voiceLabelCounts[voiceLabel] = (activeDay.voiceLabelCounts[voiceLabel] || 0) + 1; }
      await writeJson(DAY_KEYS[activeDay.dayNumber - 1], activeDay); lastSampleAt = now;
    }
  }
  await writeMeta({ ...meta, latestTelemetry: telemetry, lastSeenTimestamp: now, lastSampleAt, previousAccel: motionCalc.next, stage: effectiveStage, heldStage });
  return json({ ok: true, receivedAt: now, stage: effectiveStage, latestTelemetry: telemetry, stageChanged: (meta.stage?.stageCode ?? 0) !== effectiveStage.stageCode });
}

async function handleGuideGeneration(req) {
  if (!accessAuthorized(req)) return json({ ok: false, error: '보호자 인증이 필요합니다.' }, 401);
  const days = await loadDays();
  const analytics = calculateAnalytics(days);
  if (!analytics.is14DaysCompleted) return json({ ok: false, error: `14일 실제 수집이 완료되지 않았습니다. 현재 ${analytics.completedDaysCount}/14일입니다.`, daysCollected: analytics.completedDaysCount, daysRemaining: analytics.daysRemaining }, 400);
  const apiKey = process.env.GEMINI_API_KEY;
  if (!apiKey) return json({ ok: false, error: 'GEMINI_API_KEY가 Vercel 환경변수에 없습니다.' }, 500);
  const meta = await readMeta();
  const currentStage = meta.stage || evaluateStage(0, 0, 'NORMAL', undefined, undefined, { calmBpm: analytics.realStats.heartRate.calmAverage, elevatedThreshold: analytics.realStats.heartRate.elevatedThreshold });
  const body = await req.json().catch(() => ({}));
  const parentCalmingPreferences = Array.isArray(body?.parentCalmingPreferences) ? body.parentCalmingPreferences : [];
  const prompt = `
당신은 CALM FIT의 보호자·교사 지원용 가이드 생성 AI입니다. 의료 진단이나 치료를 하지 마세요.

가장 중요한 규칙:
1. 아래 [실제 통계]에 없는 숫자, 사건, 패턴, 원인, 효과를 절대 만들어내지 마세요.
2. 통계가 확인되지 않은 것을 관찰되었다고 단정하지 마세요.
3. 실제 측정값과 AI의 해석을 명확히 구분하세요.
4. 현재 상태가 주의/경고/매우 위험이면 그 현재 상태에 바로 참고할 수 있는 지원 행동을 currentStateGuide에 먼저 작성하세요.
5. 어떤 행동도 모든 아동에게 효과가 있다고 보장하지 마세요.
6. 신체 접촉, 압박, 특정 도구 등은 아이가 평소 허용했고 보호자가 이미 사용하던 경우처럼 조건을 붙여 안전하게 표현하세요.
7. 현재 심박수나 움직임 값은 실시간 측정값이며 진단 결과가 아닙니다.
8. 데이터가 없는 항목은 null 또는 빈 배열로 반환하세요.

[14일 실제 통계]
${JSON.stringify(analytics.realStats, null, 2)}

[현재 실제 센서 상태]
${JSON.stringify({ stage: currentStage, latestTelemetry: meta.latestTelemetry }, null, 2)}

[보호자 선호 지원 방법]
${JSON.stringify(parentCalmingPreferences, null, 2)}

반드시 순수 JSON만 반환하세요:
{
  "childProfileSummary": "실제 14일 통계만을 바탕으로 작성한 관찰 요약",
  "baseline": { "calmBpm": ${JSON.stringify(analytics.realStats.heartRate.calmAverage)}, "elevatedThreshold": ${JSON.stringify(analytics.realStats.heartRate.elevatedThreshold)}, "primarySensoryTriggers": [] },
  "currentStateGuide": { "stageCode": ${currentStage.stageCode}, "stageName": ${JSON.stringify(currentStage.stageName)}, "actions": [{"title":"현재 상태에서 참고할 행동","desc":"실제 데이터에 없는 효과를 보장하지 않는 표현","tag":"지원"}] },
  "stages": {
    "0": {"stageName":"0단계: 안정 (Stable)","badge":"안정 상태","summary":"","actions":[]},
    "1": {"stageName":"1단계: 주의 (Caution)","badge":"주의","summary":"","actions":[]},
    "2": {"stageName":"2단계: 경고 (Warning)","badge":"주의 깊은 지원 필요","summary":"","actions":[]},
    "3": {"stageName":"3단계: 매우 위험 (Danger)","badge":"안전 우선","summary":"","actions":[]}
  },
  "topCalmingMethods": []
}`;
  try {
    const ai = new GoogleGenAI({ apiKey });
    const response = await ai.models.generateContent({ model: 'gemini-2.5-flash', contents: prompt, config: { responseMimeType: 'application/json' } });
    const parsed = JSON.parse((response.text || '').replace(/```json/g, '').replace(/```/g, '').trim());
    const guide = normalizeGuide(parsed, analytics, currentStage);
    await writeGuide(guide);
    return json({ ok: true, guide, modelUsed: 'gemini-2.5-flash', isFallback: false });
  } catch (error) {
    console.error('[CALM FIT] Gemini generation failed:', error);
    return json({ ok: false, error: 'AI 맞춤 가이드 생성에 실패했습니다. AI가 생성하지 않은 결과를 AI 결과로 표시하지 않습니다.', isFallback: false }, 500);
  }
}

async function handleCompleteDay(req) {
  if (!accessAuthorized(req)) return json({ ok: false, error: '보호자 인증이 필요합니다.' }, 401);
  const body = await req.json().catch(() => ({}));
  const requestedDay = Number(body?.dayNumber);
  const days = await loadDays();
  const active = days.find(d => !d.isCompleted);
  if (!active || requestedDay !== active.dayNumber) return json({ ok: false, error: `현재 완료할 수 있는 일차는 ${active?.dayNumber || 14}일차입니다.` }, 400);
  if (!active.samples.length) return json({ ok: false, error: '해당 일차에 실제 센서 샘플이 없습니다.' }, 400);
  active.isCompleted = true; active.completedAt = Date.now();
  if (typeof body?.situationTag === 'string' && body.situationTag.trim()) active.situationTag = body.situationTag.trim();
  if (typeof body?.notes === 'string' && body.notes.trim()) active.notes = body.notes.trim();
  await writeJson(DAY_KEYS[active.dayNumber - 1], active);
  const analytics = calculateAnalytics(days);
  const meta = await readMeta();
  const personalizedBaseline = analytics.is14DaysCompleted && typeof analytics.realStats?.heartRate?.calmAverage === 'number' && typeof analytics.realStats?.heartRate?.elevatedThreshold === 'number'
    ? { calmBpm: analytics.realStats.heartRate.calmAverage, elevatedThreshold: analytics.realStats.heartRate.elevatedThreshold } : null;
  await writeMeta({ ...meta, personalizedBaseline, collectionLocked: analytics.is14DaysCompleted });
  return json({ ok: true, data: days, analytics });
}

async function handleReset(req) {
  if (!accessAuthorized(req)) return json({ ok: false, error: '보호자 인증이 필요합니다.' }, 401);
  for (const key of DAY_KEYS) await blobDelete(key);
  await blobDelete('guide-current'); await blobDelete('action-logs'); await blobDelete('meta');
  const days = await ensureDaysInitialized();
  return json({ ok: true, data: days });
}

async function handleActionLog(req) {
  if (!accessAuthorized(req)) return json({ ok: false, error: '보호자 인증이 필요합니다.' }, 401);
  const body = await req.json().catch(() => ({}));
  const entry = { id: `act_${Date.now()}_${Math.random().toString(36).slice(2, 6)}`, timestamp: Date.now(), stageName: body?.stageName || '안정', actionTaken: body?.actionTaken || '보호자 행동 지침 수행', motionLevel: body?.motionLevel || 'NORMAL' };
  const logs = await readActionLogs(); logs.unshift(entry); await writeActionLogs(logs); return json({ ok: true, entry });
}

async function handleRequest(req) {
  const url = new URL(req.url);
  const path = url.pathname.replace(/^\/api/, '') || '/';
  try {
    if (req.method === 'OPTIONS') return new Response(null, { status: 204 });
    if (path === '/health' && req.method === 'GET') {
      const meta = await readMeta(); const now = Date.now();
      return json({ ok: true, status: 'healthy', message: 'CALM FIT Vercel API is running', timestamp: now, deviceConnected: meta.lastSeenTimestamp > 0 && now - meta.lastSeenTimestamp < DEVICE_TIMEOUT_MS, lastSeen: meta.lastSeenTimestamp || null, latestTelemetry: meta.latestTelemetry || null, functionsMode: true });
    }
    if (path === '/server-info' && req.method === 'GET') return json({ ok: true, production: true, secureTransportRequired: true, recommendedUrl: `${url.origin}/api/telemetry`, webSocketUrl: null, transport: 'https-polling' });
    if (path === '/local-setup-token') return json({ ok: false, error: '원격 Vercel 배포에서는 로컬 자동 토큰 발급을 사용하지 않습니다.' }, 404);
    if (path === '/telemetry' && req.method === 'POST') return await handleTelemetry(req);
    if (path === '/state' && req.method === 'GET') {
      const meta = await readMeta(); const guide = (await readGuide()) || STANDARD_BASE_GUIDE; const now = Date.now();
      return json({ ok: true, latestTelemetry: meta.latestTelemetry || null, stage: meta.stage || evaluateStage(0, 0, 'NORMAL'), activeGuide: guide, isCustomGuide: Boolean(guide?.isCustom), isDeviceConnected: meta.lastSeenTimestamp > 0 && now - meta.lastSeenTimestamp < DEVICE_TIMEOUT_MS, lastSeen: meta.lastSeenTimestamp || null });
    }
    if (path === '/stage' && req.method === 'GET') { const meta = await readMeta(); const now = Date.now(); return json({ ok: true, stage: meta.stage, latestTelemetry: meta.latestTelemetry, isDeviceConnected: meta.lastSeenTimestamp > 0 && now - meta.lastSeenTimestamp < DEVICE_TIMEOUT_MS }); }
    if (path === '/14-day/data' && req.method === 'GET') return json({ ok: true, data: await loadDays() });
    if (path === '/analytics/14-day' && req.method === 'GET') return json({ ok: true, analytics: calculateAnalytics(await loadDays()) });
    if (path === '/14-day/complete-day' && req.method === 'POST') return await handleCompleteDay(req);
    if (path === '/14-day/reset' && req.method === 'POST') return await handleReset(req);
    if (path === '/guide/active' && req.method === 'GET') { const guide = (await readGuide()) || STANDARD_BASE_GUIDE; return json({ ok: true, isCustom: Boolean(guide?.isCustom), guide }); }
    if (path === '/gemini/generate-guide' && req.method === 'POST') return await handleGuideGeneration(req);
    if (path === '/guide/action-log' && req.method === 'POST') return await handleActionLog(req);
    if (path === '/guide/action-logs' && req.method === 'GET') return json({ ok: true, logs: await readActionLogs() });
    if (path === '/telemetry/stream') return json({ ok: false, error: 'Vercel 배포에서는 지속적인 SSE 대신 /api/state 폴링을 사용합니다.' }, 410);
    return json({ ok: false, error: 'API route not found', path }, 404);
  } catch (error) {
    console.error('[CALM FIT] API error:', error);
    return json({ ok: false, error: error?.message || '서버 오류' }, 500);
  }
};

export default { fetch: handleRequest };

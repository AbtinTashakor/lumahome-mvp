const elements = {
  backendHealth: document.querySelector("#backend-health"),
  deviceStatus: document.querySelector("#device-status"),
  deviceMessageTitle: document.querySelector("#device-message-title"),
  deviceMessageDetail: document.querySelector("#device-message-detail"),
  error: document.querySelector("#request-error"),
  modeSummary: document.querySelector("#mode-summary"),
  lightSummary: document.querySelector("#light-summary"),
  desiredMode: document.querySelector("#desired-mode"),
  desiredLight: document.querySelector("#desired-light"),
  reportedMode: document.querySelector("#reported-mode"),
  reportedLight: document.querySelector("#reported-light"),
  sensorValue: document.querySelector("#sensor-value"),
  reportedVersion: document.querySelector("#reported-version"),
  desiredVersion: document.querySelector("#desired-version"),
  lastSeen: document.querySelector("#last-seen"),
  eventsEmpty: document.querySelector("#events-empty"),
  recentEvents: document.querySelector("#recent-events"),
  modeManual: document.querySelector("#mode-manual"),
  modeNight: document.querySelector("#mode-night"),
  lightOn: document.querySelector("#light-on"),
  lightOff: document.querySelector("#light-off"),
};

let currentState = null;
let requestInFlight = false;

function technicalTerm(text) {
  const term = document.createElement("span");
  term.className = "technical-term";
  term.dir = "ltr";
  term.textContent = text;
  return term;
}

function setRichText(element, parts) {
  element.replaceChildren(
    ...parts.map((part) =>
      typeof part === "string" ? document.createTextNode(part) : part,
    ),
  );
}

function setSelected(element, selected) {
  element.classList.toggle("is-selected", selected);
  element.setAttribute("aria-pressed", String(selected));
}

function renderState(state) {
  currentState = state;
  const manualMode = state.desired.mode === "manual";
  const manualLightOn = state.desired.manual_light_on;

  elements.backendHealth.textContent = "آنلاین";
  elements.backendHealth.className = "health health-online";
  elements.deviceStatus.textContent = state.online
    ? "دستگاه آنلاین است"
    : "دستگاه آفلاین است";
  elements.deviceStatus.className = state.online
    ? "badge badge-online"
    : "badge badge-offline";
  if (state.online) {
    elements.deviceMessageTitle.textContent = "دستگاه متصل است.";
    elements.deviceMessageDetail.textContent = "مقادیر گزارش‌شده به‌روز هستند.";
  } else if (state.reported) {
    setRichText(elements.deviceMessageTitle, [
      "مهلت ",
      technicalTerm("Heartbeat"),
      " دستگاه به پایان رسیده است.",
    ]);
    elements.deviceMessageDetail.textContent = "آخرین مقادیر گزارش‌شده دستگاه نمایش داده می‌شوند.";
  } else {
    setRichText(elements.deviceMessageTitle, [
      "هنوز ",
      technicalTerm("Heartbeat"),
      " از دستگاه دریافت نشده است.",
    ]);
    elements.deviceMessageDetail.textContent = "مقادیر گزارش‌شده هنوز در دسترس نیستند.";
  }
  elements.modeSummary.textContent = manualMode
    ? "حالت انتخاب‌شده: دستی"
    : "حالت انتخاب‌شده: شب";
  elements.lightSummary.textContent = manualMode
    ? manualLightOn
      ? "وضعیت انتخاب‌شده: روشن"
      : "وضعیت انتخاب‌شده: خاموش"
    : "در حالت شب در دسترس نیست";
  elements.desiredMode.textContent = manualMode ? "دستی" : "شب";
  elements.desiredLight.textContent = manualLightOn ? "روشن" : "خاموش";
  elements.reportedMode.textContent = state.reported
    ? state.reported.mode === "manual"
      ? "دستی"
      : "شب"
    : "—";
  elements.reportedLight.textContent = state.reported
    ? state.reported.light_on
      ? "روشن"
      : "خاموش"
    : "—";
  elements.sensorValue.textContent = state.reported?.sensor_raw ?? "—";
  elements.reportedVersion.textContent = state.reported?.config_version ?? "—";
  elements.desiredVersion.textContent = state.desired.config_version;
  elements.lastSeen.textContent = state.last_seen ?? "—";

  elements.modeManual.disabled = false;
  elements.modeNight.disabled = false;
  elements.lightOn.disabled = !manualMode;
  elements.lightOff.disabled = !manualMode;

  setSelected(elements.modeManual, manualMode);
  setSelected(elements.modeNight, !manualMode);
  setSelected(elements.lightOn, manualMode && manualLightOn);
  setSelected(elements.lightOff, manualMode && !manualLightOn);
}

function eventValue(event) {
  if (event.type === "mode_changed") {
    return event.mode === "manual" ? "دستی" : "شب";
  }
  if (event.type === "light_changed") {
    return event.light_on ? "روشن" : "خاموش";
  }
  return event.connected ? "متصل" : "قطع‌شده";
}

function eventLabel(type) {
  if (type === "mode_changed") {
    return "تغییر حالت";
  }
  if (type === "light_changed") {
    return "تغییر وضعیت چراغ";
  }
  return ["تغییر ارتباط ", technicalTerm("Wi-Fi")];
}

function renderEvents(events) {
  elements.recentEvents.replaceChildren();
  elements.eventsEmpty.hidden = events.length > 0;
  elements.eventsEmpty.textContent = "هیچ فعالیتی از دستگاه دریافت نشده است.";

  for (const event of events) {
    const item = document.createElement("li");
    const summary = document.createElement("span");
    const receivedAt = document.createElement("time");

    const label = eventLabel(event.type);
    const labelParts = Array.isArray(label) ? label : [label];
    setRichText(summary, [...labelParts, " — ", eventValue(event)]);
    receivedAt.dateTime = event.received_at;
    receivedAt.textContent = new Date(event.received_at).toLocaleString(
      "fa-IR-u-nu-latn",
    );
    item.append(summary, receivedAt);
    elements.recentEvents.append(item);
  }
}

function renderUnavailable() {
  currentState = null;
  elements.backendHealth.textContent = "آفلاین";
  elements.backendHealth.className = "health health-offline";
  elements.deviceStatus.textContent = "وضعیت دستگاه نامشخص است";
  elements.deviceStatus.className = "badge badge-unknown";
  setRichText(elements.deviceMessageTitle, ["وضعیت ", technicalTerm("Server"), " در دسترس نیست."]);
  elements.deviceMessageDetail.textContent = "امکان تأیید وضعیت فعلی دستگاه وجود ندارد.";
  elements.modeSummary.textContent = "در دسترس نیست";
  elements.lightSummary.textContent = "در دسترس نیست";
  elements.desiredMode.textContent = "در دسترس نیست";
  elements.desiredLight.textContent = "در دسترس نیست";
  elements.reportedMode.textContent = "—";
  elements.reportedLight.textContent = "—";
  elements.sensorValue.textContent = "—";
  elements.reportedVersion.textContent = "—";
  elements.desiredVersion.textContent = "—";
  elements.lastSeen.textContent = "—";
  elements.recentEvents.replaceChildren();
  elements.eventsEmpty.hidden = false;
  elements.eventsEmpty.textContent = "فعالیت‌های اخیر در دسترس نیستند.";

  for (const control of [
    elements.modeManual,
    elements.modeNight,
    elements.lightOn,
    elements.lightOff,
  ]) {
    control.disabled = true;
    setSelected(control, false);
  }
}

function showError(message) {
  elements.error.textContent = message;
  elements.error.hidden = false;
}

function clearError() {
  elements.error.textContent = "";
  elements.error.hidden = true;
}

async function apiRequest(path, options = {}) {
  let response;
  try {
    response = await fetch(path, {
      ...options,
      headers: {
        Accept: "application/json",
        ...(options.body ? { "Content-Type": "application/json" } : {}),
      },
    });
  } catch {
    const error = new Error("ارتباط با Server برقرار نشد.");
    error.backendReachable = false;
    throw error;
  }
  const data = await response.json().catch(() => null);

  if (!response.ok) {
    const error = new Error(
      `درخواست ناموفق بود (کد ${response.status}).`,
    );
    error.backendReachable = true;
    throw error;
  }

  return data;
}

async function loadState() {
  if (requestInFlight) {
    return;
  }

  requestInFlight = true;
  try {
    const [stateResult, eventsResult] = await Promise.allSettled([
      apiRequest("/api/state"),
      apiRequest("/api/events?limit=5"),
    ]);
    if (stateResult.status === "rejected") {
      throw stateResult.reason;
    }
    if (eventsResult.status === "rejected") {
      throw eventsResult.reason;
    }
    renderState(stateResult.value);
    renderEvents(eventsResult.value.events);
    clearError();
  } catch (error) {
    renderUnavailable();
    showError(error.message || "ارتباط با Server برقرار نشد.");
  } finally {
    requestInFlight = false;
  }
}

async function updateState(path, body) {
  if (requestInFlight) {
    return;
  }

  requestInFlight = true;
  try {
    renderState(
      await apiRequest(path, {
        method: "POST",
        body: JSON.stringify(body),
      }),
    );
    clearError();
  } catch (error) {
    if (error.backendReachable && currentState) {
      renderState(currentState);
    } else {
      renderUnavailable();
    }
    showError(error.message || "درخواست ناموفق بود.");
  } finally {
    requestInFlight = false;
  }
}

elements.modeManual.addEventListener("click", () =>
  updateState("/api/mode", { mode: "manual" }),
);
elements.modeNight.addEventListener("click", () =>
  updateState("/api/mode", { mode: "night" }),
);
elements.lightOn.addEventListener("click", () =>
  updateState("/api/light", { on: true }),
);
elements.lightOff.addEventListener("click", () =>
  updateState("/api/light", { on: false }),
);

void loadState();
setInterval(() => void loadState(), 2_000);

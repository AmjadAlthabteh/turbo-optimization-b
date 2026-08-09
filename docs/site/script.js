const tabs = document.querySelectorAll(".tab");
const panels = document.querySelectorAll(".tab-panel");
const welcomeModal = document.querySelector("#welcomeModal");
const saveWelcome = document.querySelector("#saveWelcome");
const skipWelcome = document.querySelector("#skipWelcome");
const visitorFocus = document.querySelector("#visitorFocus");
const visitorColor = document.querySelector("#visitorColor");

function readStoredJson(key, fallback) {
  try {
    return JSON.parse(localStorage.getItem(key) || JSON.stringify(fallback));
  } catch {
    return fallback;
  }
}

const savedPrefs = readStoredJson("turboPrefs", null);

function applyVisitorPrefs(prefs) {
  if (!prefs) return;

  document.body.dataset.accent = prefs.color;

  const generatedCommand = document.querySelector("#generatedCommand");
  const runPlan = document.querySelector("#runPlan");
  if (!generatedCommand || !runPlan) return;

  const focusCommands = {
    speed: {
      command: 'turbobuild optimize --project .\\project --goal speed --runs 100 --warmups 5 --benchmark-command ".\\app.exe"',
      plan: "Focus: speed -> scan project -> build O2/O3/LTO -> benchmark p95 and p99 -> write summary",
    },
    size: {
      command: 'turbobuild optimize --project .\\project --goal size --runs 80 --warmups 3 --benchmark-command ".\\app.exe"',
      plan: "Focus: binary size -> test Os/Oz/LTO -> compare artifact bytes -> keep portability notes",
    },
    safety: {
      command: "turbobuild warnings --project .\\project --strict",
      plan: "Focus: correctness -> run warnings -> sanitizer probe -> categorize risky lines -> write reports",
    },
    ci: {
      command: "turbobuild init-ci --project .\\project",
      plan: "Focus: CI -> create workflow -> run doctor, warnings, static analysis -> upload reports",
    },
  };

  generatedCommand.textContent = focusCommands[prefs.focus].command;
  runPlan.textContent = focusCommands[prefs.focus].plan;
}

if (savedPrefs) {
  applyVisitorPrefs(savedPrefs);
} else if (welcomeModal) {
  welcomeModal.classList.add("visible");
  welcomeModal.setAttribute("aria-hidden", "false");
}

if (saveWelcome && visitorFocus && visitorColor && welcomeModal) {
  saveWelcome.addEventListener("click", () => {
    const prefs = {
      focus: visitorFocus.value,
      color: visitorColor.value,
    };

    localStorage.setItem("turboPrefs", JSON.stringify(prefs));
    applyVisitorPrefs(prefs);
    welcomeModal.classList.remove("visible");
    welcomeModal.setAttribute("aria-hidden", "true");
  });
}

if (skipWelcome && welcomeModal) {
  skipWelcome.addEventListener("click", () => {
    localStorage.setItem("turboPrefs", JSON.stringify({ focus: "speed", color: "green" }));
    welcomeModal.classList.remove("visible");
    welcomeModal.setAttribute("aria-hidden", "true");
  });
}

tabs.forEach((tab) => {
  tab.addEventListener("click", () => {
    const target = tab.dataset.tab;

    tabs.forEach((item) => item.classList.toggle("active", item === tab));
    panels.forEach((panel) => {
      panel.classList.toggle("active", panel.id === target);
    });
  });
});

const codeTabs = document.querySelectorAll(".code-tab");
const codePanels = document.querySelectorAll(".code-panel");

codeTabs.forEach((tab) => {
  tab.addEventListener("click", () => {
    const target = tab.dataset.code;

    codeTabs.forEach((item) => item.classList.toggle("active", item === tab));
    codePanels.forEach((panel) => {
      panel.classList.toggle("active", panel.id === target);
    });
  });
});

const profileSelect = document.querySelector("#profileSelect");
const goalSelect = document.querySelector("#goalSelect");
const benchCommand = document.querySelector("#benchCommand");
const buildCommand = document.querySelector("#buildCommand");
const generatedCommand = document.querySelector("#generatedCommand");
const runPlan = document.querySelector("#runPlan");

const profilePlans = {
  service: "Analyze service layout -> run strict warnings -> run CTest -> benchmark p95 latency -> compare GCC/Clang",
  engine: "Scan engine modules -> check sanitizer support -> benchmark frame workload -> inspect cache and branch findings",
  embedded: "Analyze binary outputs -> test Os/Oz/LTO configs -> compare text-section size -> report portability risks",
};

function updateGeneratedCommand() {
  if (!profileSelect || !goalSelect || !benchCommand || !generatedCommand || !runPlan) return;

  const profile = profileSelect.value;
  const goal = goalSelect.value;
  const command = benchCommand.value.trim() || ".\\app.exe";
  const runs = profile === "engine" ? 200 : 100;
  const warmups = profile === "embedded" ? 3 : 5;

  generatedCommand.textContent = `turbobuild optimize --project .\\project --goal ${goal} --runs ${runs} --warmups ${warmups} --benchmark-command "${command}"`;
  runPlan.textContent = profilePlans[profile];
}

if (profileSelect && goalSelect && benchCommand && buildCommand) {
  [profileSelect, goalSelect, benchCommand].forEach((input) => {
    input.addEventListener("input", updateGeneratedCommand);
  });

  buildCommand.addEventListener("click", updateGeneratedCommand);
}

async function copyText(text, button) {
  try {
    await navigator.clipboard.writeText(text);
    button.textContent = "Copied";
    setTimeout(() => {
      button.textContent = button.dataset.label || "Copy";
    }, 1200);
  } catch {
    button.textContent = "Select";
  }
}

document.querySelectorAll(".command-card, .quickstart-card").forEach((card) => {
  const button = card.querySelector(".copy-button");
  const code = card.querySelector("code");
  if (!button || !code) return;
  button.dataset.label = button.textContent;

  button.addEventListener("click", async () => {
    await copyText(code.textContent.trim(), button);
  });
});

const copyGeneratedCommand = document.querySelector("#copyGeneratedCommand");

if (copyGeneratedCommand && generatedCommand) {
  copyGeneratedCommand.dataset.label = copyGeneratedCommand.textContent;
  copyGeneratedCommand.addEventListener("click", async () => {
    await copyText(generatedCommand.textContent.trim(), copyGeneratedCommand);
  });
}

const uploadInput = document.querySelector("#projectUpload");
const uploadStatus = document.querySelector("#uploadStatus");
const uploadBox = document.querySelector("#uploadBox");
const uploadGoal = document.querySelector("#uploadGoal");
const uploadBenchmark = document.querySelector("#uploadBenchmark");
const scanPreview = document.querySelector("#scanPreview");

function showSelectedProject(file) {
  if (!file) {
    uploadStatus.textContent = "No project selected.";
    scanPreview.classList.remove("ready");
    return;
  }

  const sizeMb = (file.size / 1024 / 1024).toFixed(2);
  const goal = uploadGoal.value;
  const command = uploadBenchmark.value.trim() || ".\\app.exe";
  uploadStatus.textContent = `${file.name} selected (${sizeMb} MB). Intake queued for ${goal}: source discovery, build-system detection, tests, sanitizer plan, and benchmark command "${command}".`;
  scanPreview.classList.add("ready");
}

if (uploadInput && uploadStatus && uploadBox && uploadGoal && uploadBenchmark && scanPreview) {
  uploadInput.addEventListener("change", () => {
    showSelectedProject(uploadInput.files[0]);
  });

  [uploadGoal, uploadBenchmark].forEach((input) => {
    input.addEventListener("input", () => {
      showSelectedProject(uploadInput.files[0]);
    });
  });

  ["dragenter", "dragover"].forEach((eventName) => {
    uploadBox.addEventListener(eventName, (event) => {
      event.preventDefault();
      uploadBox.classList.add("dragging");
    });
  });

  ["dragleave", "drop"].forEach((eventName) => {
    uploadBox.addEventListener(eventName, (event) => {
      event.preventDefault();
      uploadBox.classList.remove("dragging");
    });
  });

  uploadBox.addEventListener("drop", (event) => {
    showSelectedProject(event.dataTransfer.files[0]);
  });
}

const waitlistForm = document.querySelector("#waitlistForm");
const formStatus = document.querySelector("#formStatus");
const ideaInput = document.querySelector("#ideaInput");
const codeInput = document.querySelector("#codeInput");
const simulateCode = document.querySelector("#simulateCode");
const sandboxOutput = document.querySelector("#sandboxOutput");

function simulateCodeReview() {
  if (!ideaInput || !codeInput || !sandboxOutput) return;

  const idea = ideaInput.value.trim() || "C++ project";
  const code = codeInput.value.toLowerCase();
  const findings = [];

  if (code.includes("push_back")) findings.push("Finding: repeated vector growth is likely; measure reserve().");
  if (code.includes("std::endl")) findings.push("Finding: std::endl flushes output; measure '\\n' for hot logs.");
  if (code.includes("new ") || code.includes("delete ")) findings.push("Finding: manual lifetime management; inspect ownership and leaks.");
  if (code.includes("string")) findings.push("Finding: string-heavy path; check copies and string_view candidates.");
  if (!findings.length) findings.push("Finding: no obvious heuristic hit; run benchmark and warnings anyway.");

  sandboxOutput.textContent = [
    `Project idea: ${idea}`,
    ...findings,
    "Command: turbobuild doctor --project .",
    'Next: turbobuild benchmark --runs 100 --warmups 5 --command ".\\app.exe"',
  ].join("\n");
}

if (simulateCode) {
  simulateCode.addEventListener("click", simulateCodeReview);
}

if (waitlistForm && formStatus) {
  waitlistForm.addEventListener("submit", (event) => {
    event.preventDefault();

    const formData = new FormData(waitlistForm);
    const entry = {
      email: formData.get("email"),
      projectType: formData.get("projectType"),
      goal: formData.get("goal"),
      createdAt: new Date().toISOString(),
    };

    const current = readStoredJson("turboWaitlist", []);
    current.push(entry);
    localStorage.setItem("turboWaitlist", JSON.stringify(current));

    formStatus.textContent = "Saved. Opening confirmation...";
    window.location.href = "waitlist.html";
  });
}

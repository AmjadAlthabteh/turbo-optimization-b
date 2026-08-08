const tabs = document.querySelectorAll(".tab");
const panels = document.querySelectorAll(".tab-panel");

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

document.querySelectorAll(".command-card").forEach((card) => {
  const button = card.querySelector(".copy-button");
  const command = card.querySelector("code").textContent.trim();

  button.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(command);
      button.textContent = "Copied";
      setTimeout(() => {
        button.textContent = "Copy";
      }, 1200);
    } catch {
      button.textContent = "Select";
    }
  });
});

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

waitlistForm.addEventListener("submit", (event) => {
  event.preventDefault();

  const formData = new FormData(waitlistForm);
  const entry = {
    email: formData.get("email"),
    projectType: formData.get("projectType"),
    goal: formData.get("goal"),
    createdAt: new Date().toISOString(),
  };

  const current = JSON.parse(localStorage.getItem("turboWaitlist") || "[]");
  current.push(entry);
  localStorage.setItem("turboWaitlist", JSON.stringify(current));

  waitlistForm.reset();
  formStatus.textContent = "Request saved locally for this preview.";
});

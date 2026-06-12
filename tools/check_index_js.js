/* Syntax-check every inline <script> block in main/web/index.html.
 * A single SyntaxError silently kills the whole editor (every function
 * undefined), so run this after hand edits. */
const fs = require("fs");
const path = require("path");
const file = path.join(__dirname, "..", "main", "web", "index.html");
const html = fs.readFileSync(file, "utf8");
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];
let ok = true;
scripts.forEach((m, i) => {
  try {
    new Function(m[1]);
  } catch (e) {
    ok = false;
    console.log("script block " + i + ": " + e.message);
  }
});
console.log(ok ? "JS OK (" + scripts.length + " blocks)" : "JS FAIL");
process.exit(ok ? 0 : 1);

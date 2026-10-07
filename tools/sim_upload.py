"""Firmware uploaded from the browser.

This one writes to the other half of the flash and then boots from it,
over a hotspot whose password is in a public repository, so the
interesting question is not whether it works but what it refuses.
localUploadChunk is lifted out of the sketch, compiled against stub
versions of Update and the web server, and driven through the cases
that matter: nobody authorised, a file that is not firmware, a file
too big for the slot, an upload cut off halfway, and a good one.

The ordering matters more than any single check. The completion
handler runs only after every byte has landed, so authorisation has to
be tested in the streaming callback; testing it in the handler would
mean the image was already in the spare slot before anyone asked who
sent it.
"""
import re, sys, os, subprocess, tempfile

src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

def grab(sig):
    i = src.index(sig); depth = 0; out = []
    for ch in src[i:]:
        out.append(ch)
        if ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: break
    return "".join(out)

GLOBALS = re.search(r"(bool   upLocal = false;.*?String upErr = \"\";)", src, re.S).group(1)
CHUNK = GLOBALS + "\n\n" + grab("static void localUploadChunk() {")

HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <string>
struct String {
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& o) : s(o) {}
  size_t length() const { return s.size(); }
  long toInt() const { return atol(s.c_str()); }
  const char* c_str() const { return s.c_str(); }
  String& operator=(const char* c) { s = c ? c : ""; return *this; }
  bool operator==(const char* c) const { return s == c; }
};
static long constrain_(long v, long a, long b){ return v<a?a:(v>b?b:v); }
#define constrain(v,a,b) constrain_((v),(a),(b))

enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload { int status; size_t currentSize; uint8_t* buf; String filename; };

static HTTPUpload UP;
static String HDR_SIZE;
static int  AUTH = 1;
static size_t SLOT = 1900544;
static int  SLOT_OK = 1;

struct WebStub {
  HTTPUpload& upload() { return UP; }
  String header(const char* n) { return (!strcmp(n,"X-Rafiq-Size")) ? HDR_SIZE : String(""); }
} web;
static bool authed() { return AUTH != 0; }

struct UpdateStub {
  bool open = false; size_t written = 0; int begins = 0, aborts = 0, ends = 0;
  bool endOk = true; size_t cap = 0;
  bool begin(size_t n) { begins++; cap = n; open = true; return true; }
  size_t write(uint8_t* b, size_t n) { if (!open) return 0; written += n; return n; }
  void abort() { aborts++; open = false; }
  bool end(bool b) { ends++; open = false; return endOk; }
} Update;
#define UPDATE_SIZE_UNKNOWN ((size_t)0xFFFFFFFF)

struct esp_partition_t { size_t size; };
static esp_partition_t SLOTP;
static const esp_partition_t* esp_ota_get_next_update_partition(void*) {
  if (!SLOT_OK) return 0;
  SLOTP.size = SLOT; return &SLOTP;
}
static String otaStatus, otaStatus2;
static int otaPct = -1;
static bool asleep = false;
static void wake(const char*) {}
static int draws = 0;
static void drawOta() { draws++; }

@@CHUNK@@

static uint8_t BUF[4096];
int main(void) {
  char line[9000];
  while (fgets(line, sizeof line, stdin)) {
    char* nl = strchr(line, '\n'); if (nl) *nl = 0;
    char* sp = strchr(line, ' ');
    char* arg = sp ? sp + 1 : (char*)"";
    if (sp) *sp = 0;
    if (!strcmp(line, "AUTH"))      { AUTH = atoi(arg); printf("OK\n"); }
    else if (!strcmp(line, "SIZE")) { HDR_SIZE = arg;  printf("OK\n"); }
    else if (!strcmp(line, "SLOT")) { SLOT = (size_t)atol(arg); printf("OK\n"); }
    else if (!strcmp(line, "NOSLOT")) { SLOT_OK = 0;   printf("OK\n"); }
    else if (!strcmp(line, "ENDFAIL")) { Update.endOk = false; printf("OK\n"); }
    else if (!strcmp(line, "START")) { UP.status = UPLOAD_FILE_START; UP.currentSize = 0; UP.buf = BUF; localUploadChunk(); printf("OK\n"); }
    else if (!strcmp(line, "WRITE")) {
      int n = 0;
      for (char* p = arg; p[0] && p[1]; p += 2) { char b[3]={p[0],p[1],0}; BUF[n++] = (uint8_t)strtol(b,NULL,16); }
      UP.status = UPLOAD_FILE_WRITE; UP.currentSize = n; UP.buf = BUF; localUploadChunk(); printf("OK\n");
    }
    else if (!strcmp(line, "PAD")) {   // n bytes of filler, not the first chunk
      int n = atoi(arg); if (n > 4096) n = 4096;
      memset(BUF, 0x5A, n);
      UP.status = UPLOAD_FILE_WRITE; UP.currentSize = n; UP.buf = BUF; localUploadChunk(); printf("OK\n");
    }
    else if (!strcmp(line, "END"))   { UP.status = UPLOAD_FILE_END; localUploadChunk(); printf("OK\n"); }
    else if (!strcmp(line, "ABORT")) { UP.status = UPLOAD_FILE_ABORTED; localUploadChunk(); printf("OK\n"); }
    else if (!strcmp(line, "DUMP")) {
      printf("begins=%d wrote=%zu aborts=%d ends=%d pct=%d err=%s\n",
             Update.begins, Update.written, Update.aborts, Update.ends, otaPct,
             upErr.length() ? upErr.c_str() : "-");
    }
    fflush(stdout);
  }
  return 0;
}
"""

d = tempfile.mkdtemp()
c = os.path.join(d, "u.cpp")
open(c, "w").write(HARNESS.replace("@@CHUNK@@", CHUNK))
EXE = os.path.join(d, "u")
r = subprocess.run(["c++", "-std=c++17", "-O1", "-w", "-o", EXE, c], capture_output=True, text=True)
assert r.returncode == 0, "extracted code will not compile:\n" + r.stderr[:900]

class Box:
    def __init__(self):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
    def cmd(self, c):
        self.p.stdin.write(c + "\n"); self.p.stdin.flush()
        return self.p.stdout.readline().strip()
    def dump(self):
        out = self.cmd("DUMP")
        head, err = out.split("err=", 1)
        d = dict(kv.split("=") for kv in head.split())
        d["err"] = err.strip()
        return d

GOOD_HEAD = "e9" + "00" * 31      # an ESP32 image starts 0xE9

def t_unauthorised_writes_nothing():
    b = Box(); b.cmd("AUTH 0"); b.cmd("SIZE 1000000")
    b.cmd("START"); b.cmd("WRITE " + GOOD_HEAD); b.cmd("PAD 2048"); b.cmd("END")
    d = b.dump()
    assert d["begins"] == "0", "it opened the OTA slot for an unauthorised caller"
    assert d["wrote"] == "0", f"it wrote {d['wrote']} bytes for an unauthorised caller"
    assert d["ends"] == "0", "it finalised an unauthorised image"
    assert d["err"] == "Pair first", d["err"]
    print("        nothing opened, nothing written, nothing finalised")
run("An unauthorised upload never reaches the flash", t_unauthorised_writes_nothing)

def t_auth_is_checked_while_streaming_not_after():
    """The ordering. If the check lived in the completion handler the
    bytes would already be in the slot by the time it ran."""
    start = CHUNK[CHUNK.index("UPLOAD_FILE_START"):]
    start = start[:start.index("if (u.status == UPLOAD_FILE_WRITE)")]
    assert "if (!authed())" in start, "authorisation is not checked as the bytes arrive"
    assert start.index("if (!authed())") < start.index("Update.begin"), \
        "the slot is opened before the caller is checked"
    route = src[src.index('web.on("/api/upload", HTTP_POST'):]
    route = route[:route.index("}, localUploadChunk);")]
    assert "localUploadChunk" in src[src.index('web.on("/api/upload"'):][:1200], \
        "the streaming callback is not wired to the route"
    print("        checked at the first byte, before the slot is opened")
run("The caller is checked before anything is opened", t_auth_is_checked_while_streaming_not_after)

def t_not_firmware_is_refused():
    for name, head in [("a JPEG", "ffd8ffe0" + "00" * 28),
                       ("a PNG", "89504e47" + "00" * 28),
                       ("text", "48656c6c" + "6f" * 28),
                       ("zeros", "00" * 32)]:
        b = Box(); b.cmd("AUTH 1"); b.cmd("SIZE 1000000")
        b.cmd("START"); b.cmd("WRITE " + head); b.cmd("PAD 2048"); b.cmd("END")
        d = b.dump()
        assert d["err"] == "Not a firmware file", f"{name}: {d['err']}"
        assert d["aborts"] == "1", f"{name}: the slot was not abandoned"
        assert d["ends"] == "0", f"{name}: it was finalised anyway"
        assert int(d["wrote"]) == 0, f"{name}: {d['wrote']} bytes went in"
    print("        JPEG, PNG, text and zeros all refused on the first chunk")
run("A file that is not firmware is refused before it is written", t_not_firmware_is_refused)

def t_too_big_for_the_slot():
    b = Box(); b.cmd("AUTH 1"); b.cmd("SLOT 1900544"); b.cmd("SIZE 4000000")
    b.cmd("START"); b.cmd("WRITE " + GOOD_HEAD)
    d = b.dump()
    assert d["begins"] == "0", "it opened a slot it could not fill"
    assert d["err"].startswith("Too"), d["err"]
    assert int(d["wrote"]) == 0
    print(f"        {d['err']}, and nothing opened")
run("An image too big for the slot is refused up front", t_too_big_for_the_slot)

def t_no_slot_at_all():
    b = Box(); b.cmd("AUTH 1"); b.cmd("NOSLOT"); b.cmd("SIZE 1000000")
    b.cmd("START"); b.cmd("WRITE " + GOOD_HEAD)
    d = b.dump()
    assert d["err"] == "No OTA slot", d["err"]
    assert d["begins"] == "0" and int(d["wrote"]) == 0
    print("        no slot, no write, and it says so")
run("A partition table with nowhere to put it is handled", t_no_slot_at_all)

def t_a_cut_off_upload_is_not_installed():
    b = Box(); b.cmd("AUTH 1"); b.cmd("SIZE 1000000")
    b.cmd("START"); b.cmd("WRITE " + GOOD_HEAD); b.cmd("PAD 4096"); b.cmd("ABORT")
    d = b.dump()
    assert d["aborts"] == "1", "a cancelled upload left the slot open"
    assert d["ends"] == "0", "a cancelled upload was finalised"
    assert d["err"] == "Cancelled", d["err"]
    print("        the slot is abandoned and never finalised")
run("An upload that stops halfway is abandoned", t_a_cut_off_upload_is_not_installed)

def t_a_truncated_image_fails_at_the_end():
    """end(true) is where a short image is caught. It has to be the
    thing that decides, not the next boot."""
    b = Box(); b.cmd("AUTH 1"); b.cmd("SIZE 1000000"); b.cmd("ENDFAIL")
    b.cmd("START"); b.cmd("WRITE " + GOOD_HEAD); b.cmd("PAD 4096"); b.cmd("END")
    d = b.dump()
    assert d["err"] == "Install failed", d["err"]
    assert d["ends"] == "1", "end() was not the thing that checked"
    print("        end(true) refuses it, on this boot rather than the next")
run("A truncated image is caught before it is booted", t_a_truncated_image_fails_at_the_end)

def t_a_good_one_goes_in():
    b = Box(); b.cmd("AUTH 1"); b.cmd("SIZE 8224")
    b.cmd("START"); b.cmd("WRITE " + GOOD_HEAD)
    for _ in range(4): b.cmd("PAD 2048")
    b.cmd("END")
    d = b.dump()
    assert d["err"] == "-", d["err"]
    assert d["begins"] == "1" and d["ends"] == "1" and d["aborts"] == "0"
    assert int(d["wrote"]) == 32 + 4 * 2048, d["wrote"]
    assert int(d["pct"]) >= 90, f"progress stalled at {d['pct']}%"
    print(f"        {d['wrote']} bytes in, finalised once, progress reached {d['pct']}%")
run("A real image is written and finalised", t_a_good_one_goes_in)

def t_the_page_does_not_reboot_on_a_refusal():
    h = src[src.index('web.on("/api/upload", HTTP_POST'):]
    h = h[:h.index("}, localUploadChunk);")]
    ref = h[h.index("if (upErr.length()) {"):h.index("web.send(200, \"application/json\", \"{\\\"ok\\\":true}\");")]
    assert "ESP.restart" not in ref, "it restarts even when the upload was refused"
    assert "flash(" in ref, "a refusal is only reported to the browser, not the robot"
    assert 'upErr = "";' in ref, "the error is left set and poisons the next upload"
    print("        a refusal is said out loud, and does not restart anything")
run("A refused upload does not reboot the robot", t_the_page_does_not_reboot_on_a_refusal)

def t_the_page_can_actually_send_it():
    assert 'id="fw"' in src and "sendFw()" in src, "there is no file picker on the page"
    js = src[src.index("window.sendFw=function(){"):]
    js = js[:js.index("window.locked=false;")]
    assert "X-Rafiq-Token" in js, "the upload goes up without the token, so it is refused"
    assert "X-Rafiq-Size" in js, "no size is sent, so there is no progress"
    assert "x.upload.onprogress" in js, "a few megabytes with no progress looks broken"
    assert "confirm(" in js, "it installs without asking"
    assert '"X-Rafiq-Size"' in src[src.index("const char* keep[]"):][:200], \
        "the size header is not collected, so the server never sees it"
    print("        token, size, progress and a confirmation, and the header is collected")
run("The page sends it the way the robot expects", t_the_page_can_actually_send_it)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

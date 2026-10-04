# tools

What checks the firmware before it goes out.

Run them from the directory above this one, with a `nexus-repo/` holding
the sketch and a `rafiq-app/` holding the Mac app beside it:

    for f in tools/sim_*.py tools/check_ui30.py; do python3 "$f"; done

**`sim_*.py`** read the real source and assert against it. Several of
them do not assert that a line exists, they build a small model out of
the firmware's own constants and run it: `sim_depth.py` walks the
carousel with a loop pass between every press, `sim_v52.py` holds the
pad and watches what happens, `sim_wake.py` brushes the pad and checks
the alarm survived. That is deliberate. The bug that kept the
reminders shut for three releases passed every check that only read
the gesture handler, because the gesture handler was right and the
loop undid it two milliseconds later.

**`check_ui30.py`** measures every string the panel draws against the
128 pixel width. It has caught six overflows that reading did not.

**`render_*.py`** draw screens at real pixel size so they can be
looked at before anybody flashes anything.

These lived in a scratch directory until it was partially wiped and
one of them went with it. They are here so that cannot happen again.

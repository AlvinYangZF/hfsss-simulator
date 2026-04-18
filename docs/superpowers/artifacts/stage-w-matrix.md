# Stage W Sweep Matrix

Cell format: `<verify>v/<iou>i`. Additional markers: `!json` (missing/broken JSON), `exit=N` (fio non-zero exit), `je=N` (JSON total_err > 0). Any marker or non-zero counter makes the repeat a failure for status classification.

## iodepth

| point | rep1 | rep2 | rep3 | status |
|-------|------|------|------|--------|
| 1 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 4 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 16 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 64 | 0v/0i | 0v/0i | 0v/0i | PASS |

## rwmix

| point | rep1 | rep2 | rep3 | status |
|-------|------|------|------|--------|
| 0 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 70 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 100 | 0v/0i | 0v/0i | 0v/0i | PASS |

## numjobs

| point | rep1 | rep2 | rep3 | status |
|-------|------|------|------|--------|
| 1 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 2 | 34v/0i exit=2 | 36v/0i exit=2 | 26v/0i exit=2 | FAIL |
| 4 | 178v/0i exit=4 | 157v/0i exit=4 | 124v/0i exit=4 | FAIL |

## verify_async

| point | rep1 | rep2 | rep3 | status |
|-------|------|------|------|--------|
| 0 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 2 | 0v/0i | 0v/0i | 0v/0i | PASS |
| 4 | 0v/0i | 0v/0i | 0v/0i | PASS |

## bs

| point | rep1 | rep2 | rep3 | status |
|-------|------|------|------|--------|
| 4k | 0v/64i exit=1 | 0v/64i exit=1 | 0v/64i exit=1 | FAIL |
| 16k | 0v/64i exit=1 | 0v/64i exit=1 | 0v/64i exit=1 | FAIL |

import cv2
import numpy as np

source = r"C:\Medal\Clips\Counter-Strike 2\MedalTVCounterStrike220260902141436895.mp4"
output = r"C:\Users\devbu\Desktop\improving rcs 1\KryptiK v4\rcs_clip_contact.jpg"

capture = cv2.VideoCapture(source)
frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
fps = capture.get(cv2.CAP_PROP_FPS)
width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
print(f"frames={frame_count} fps={fps:.3f} duration={frame_count / fps:.3f} size={width}x{height}")

frames = []
for frame_id in np.linspace(0, max(0, frame_count - 1), 16, dtype=int):
    capture.set(cv2.CAP_PROP_POS_FRAMES, int(frame_id))
    ok, frame = capture.read()
    if not ok:
        continue
    frame = cv2.resize(frame, (640, 360))
    cv2.putText(frame, f"{frame_id / fps:.2f}s", (12, 32), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
    frames.append(frame)

capture.release()
if len(frames) != 16:
    raise RuntimeError(f"expected 16 frames, decoded {len(frames)}")

sheet = np.vstack([np.hstack(frames[index:index + 4]) for index in range(0, 16, 4)])
if not cv2.imwrite(output, sheet):
    raise RuntimeError("failed to write contact sheet")

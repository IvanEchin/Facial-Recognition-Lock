import cv2
import numpy as np
import os
import urllib.request
import time

# ------------------ FACE DETECTOR ------------------
face_cascade = cv2.CascadeClassifier(
    cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'
)

# ------------------ LOAD TRAINING DATA ------------------
faces = []
labels = []
label_map = {}
label_id = 0

dataset_path = "train"

for person in os.listdir(dataset_path):
    person_path = os.path.join(dataset_path, person)
    label_map[label_id] = person

    for img_name in os.listdir(person_path):
        img_path = os.path.join(person_path, img_name)
        img = cv2.imread(img_path, cv2.IMREAD_GRAYSCALE)

        detected = face_cascade.detectMultiScale(img, 1.3, 5)
        for (x, y, w, h) in detected:
            face = img[y:y+h, x:x+w]
            face = cv2.resize(face, (200, 200))
            faces.append(face)
            labels.append(label_id)

    label_id += 1

# ------------------ TRAIN LBPH ------------------
recognizer = cv2.face.LBPHFaceRecognizer_create()
recognizer.train(faces, np.array(labels))

print("✓ Face recognizer trained")

# ------------------ ESP32 URLS ------------------
ESP32_IP = "192.168.1.215"
ESP32_URL = f"http://{ESP32_IP}/capture"
GRANT_ACCESS_URL = f"http://{ESP32_IP}/grant"
LED_OFF_URL = f"http://{ESP32_IP}/led/off"

# ------------------ ACCESS CONTROL STATE ------------------
led_state = False
access_granted_time = 0
ACCESS_DURATION = 10  # seconds - matches your ESP32 code
COOLDOWN_AFTER_ACCESS = 3  # seconds - prevents immediate re-trigger

def grant_access():
    """Grant access - triggers the 10-second timer on ESP32"""
    global led_state, access_granted_time
    try:
        urllib.request.urlopen(GRANT_ACCESS_URL)
        led_state = True
        access_granted_time = time.time()
        print("🟢 ACCESS GRANTED - 10 second timer started")
    except Exception as e:
        print(f"Error granting access: {e}")

def is_access_active():
    """Check if we're still in the access period"""
    if access_granted_time == 0:
        return False
    elapsed = time.time() - access_granted_time
    return elapsed < (ACCESS_DURATION + COOLDOWN_AFTER_ACCESS)

def reset_access():
    """Reset access state"""
    global led_state, access_granted_time
    led_state = False
    access_granted_time = 0

# ------------------ MAIN LOOP ------------------
print("Starting face recognition system...")
print(f"Access duration: {ACCESS_DURATION}s + {COOLDOWN_AFTER_ACCESS}s cooldown")

while True:
    try:
        img_resp = urllib.request.urlopen(ESP32_URL)
        img_arr = np.array(bytearray(img_resp.read()), dtype=np.uint8)
        frame = cv2.imdecode(img_arr, -1)

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        faces_detected = face_cascade.detectMultiScale(gray, 1.3, 5)

        recognized = False
        recognized_name = ""

        for (x, y, w, h) in faces_detected:
            face = gray[y:y+h, x:x+w]
            face = cv2.resize(face, (200, 200))

            label, confidence = recognizer.predict(face)

            # LBPH: LOWER confidence = better match
            if confidence < 60:
                name = label_map[label]
                color = (0, 255, 0)
                recognized = True
                recognized_name = name
                text = f"{name} ({confidence:.1f})"
            else:
                color = (0, 0, 255)
                text = "Unknown"

            cv2.rectangle(frame, (x,y), (x+w,y+h), color, 2)
            cv2.putText(frame, text, (x, y-10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)

        # Show access status on screen
        if is_access_active():
            elapsed = time.time() - access_granted_time
            remaining = (ACCESS_DURATION + COOLDOWN_AFTER_ACCESS) - elapsed
            status = f"ACCESS ACTIVE: {remaining:.1f}s remaining"
            status_color = (0, 255, 0)
        else:
            status = "MONITORING"
            status_color = (255, 255, 255)

        cv2.putText(frame, status, (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, status_color, 2)

        # Grant access logic
        if recognized and not is_access_active():
            # Face recognized and not currently in access period
            grant_access()
        elif not is_access_active() and led_state:
            # Access period ended, reset state
            reset_access()
            print("⏰ Access period ended")

        cv2.imshow("ESP32 Face Recognition Lock", frame)

        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    except Exception as e:
        print(f"Error: {e}")
        time.sleep(1)

cv2.destroyAllWindows()
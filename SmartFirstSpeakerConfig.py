import socket, os, webbrowser
from gtts import gTTS
from pydub import AudioSegment

# --- CONFIGURATION ---
HOST = '0.0.0.0' 
PORT = 1234
PLAY_TRIGGER = False

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SPEECH_OUTPUT_PATH = os.path.join(BASE_DIR, 'AudioOutput', 'outputSpeech.wav')

if not os.path.exists(os.path.dirname(SPEECH_OUTPUT_PATH)):
    os.makedirs(os.path.dirname(SPEECH_OUTPUT_PATH))

def send_http_response(conn, content, ctype='text/html'):
    body = content.encode('utf-8') if isinstance(content, str) else content
    header = f"HTTP/1.1 200 OK\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n"
    conn.sendall(header.encode('utf-8') + body)

# HTML UI
ui_html = """
<html><body style='font-family:sans-serif; text-align:center; padding:50px;'>
    <h2>ESP32 TTS Controller</h2>
    <form action="/generate" method="POST">
        <input type="text" name="tts_text" placeholder="Type speech here..." style="width:300px; padding:10px;">
        <input type="submit" value="Generate & Play" style="padding:10px; background:green; color:white;">
    </form>
</body></html>
"""

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind((HOST, PORT))
s.listen(5)
print(f"Server at http://localhost:{PORT}")
webbrowser.open(f"http://localhost:{PORT}")

while True:
    conn, addr = s.accept()
    with conn:
        data = conn.recv(1024).decode('latin-1')
        if not data: continue
        
        request_line = data.split('\r\n')[0]
        method, url, _ = request_line.split()

        if method == 'GET' and url == '/':
            send_http_response(conn, ui_html)

        elif method == 'GET' and url == '/check_status':
            send_http_response(conn, "PLAY" if PLAY_TRIGGER else "WAIT", ctype='text/plain')
            PLAY_TRIGGER = False

        elif method == 'GET' and url == '/download':
            with open(SPEECH_OUTPUT_PATH, 'rb') as f:
                send_http_response(conn, f.read(), ctype='audio/wav')

        elif method == 'POST' and url == '/generate':
            # Extract text from the POST body
            try:
                tts_text = data.split('tts_text=')[1].split('&')[0].replace('+', ' ')
                print(f"Generating speech for: {tts_text}")
                
                # 1. Generate TTS
                tts = gTTS(text=tts_text, lang='en')
                tts.save(SPEECH_OUTPUT_PATH)

                # 2. Resample to prevent ESP32 OOM
                audio = AudioSegment.from_file(SPEECH_OUTPUT_PATH)
                audio = audio.set_frame_rate(22050).set_channels(1)
                audio.export(SPEECH_OUTPUT_PATH, format="wav")

                # 3. Trigger the ESP32
                PLAY_TRIGGER = True
                send_http_response(conn, "<h1>Speech Generated!</h1><p>The ESP32 is now downloading and playing...</p><a href='/'>Back</a>")
            except:
                send_http_response(conn, "Error generating speech")
import requests
import uuid

API_URL = "https://1xlhqdcbnj.execute-api.us-east-2.amazonaws.com/prod/ask"

SESSION_ID = str(uuid.uuid4())

def ask_smartfirst(message: str, show_sources: bool = False):
    payload = {
        "session_id": SESSION_ID,
        "query": message
    }

    response = requests.post(API_URL, json=payload, timeout=90)

    if response.status_code != 200:
        raise Exception(f"SmartFirst API Error: {response.text}")

    data = response.json()

    if show_sources:
        return data["response"], data.get("sources", [])
    else:
        return data["response"]
import requests
import json

API_URL = "https://1xlhqdcbnj.execute-api.us-east-2.amazonaws.com/prod/ask"

def ask_smartfirst(question: str):
    payload = {
        "query": question
    }

    try:
        response = requests.post(
            API_URL,
            headers={"Content-Type": "application/json"},
            data=json.dumps(payload),
            timeout=60
        )

        print("Status:", response.status_code)

        data = response.json()

        print("\n----- RESPONSE -----\n")
        print(data.get("response", "No response"))

        print("\n----- SOURCES -----\n")
        for s in data.get("sources", []):
            print(s)

    except Exception as e:
        print("Request failed:", e)


if __name__ == "__main__":
    ask_smartfirst("How do i do cpr?")


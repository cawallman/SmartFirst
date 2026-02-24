import json
import boto3
import os
import time
from boto3.dynamodb.conditions import Key

# ---------- AWS Clients ----------
KB_ID = os.environ["KB_ID"]

agent = boto3.client("bedrock-agent-runtime")

dynamodb = boto3.resource("dynamodb")
table = dynamodb.Table("smartfirst_sessions")


# ---------- Prompts ----------
ORCHESTRATION_PROMPT = """
You are a medical triage assistant helping route an emergency question.

User question:
$query$

Conversation:
$conversation_history$

Select only relevant information from the knowledge base.

$output_format_instructions$
"""

GENERATION_PROMPT = """
You are a medical instruction extraction system.

You may ONLY use the retrieved documents below.
Every instruction must be directly supported by the retrieved text.
Keep wording close to the source text without making it too complicated. 
Define terms and procedures that someone without training might not understand.

User question:
$query$

Retrieved documents:
$search_results$

Extract and format EXACTLY (No special characters, no dashes, this output is read using text-to-speech):

WHAT YOU SHOULD DO IMMEDIATELY: 
A few sentence paragraph explaining medical procedures to do immediately to help the situation 

WHAT YOU SHOULD DO WHILE WAITING FOR EMERGENCY SERVICES: 
A few sentence paragraph explaining what to do while waiting for emergency services to arrive
"""


# ---------- Memory Functions ----------
def save_message(session_id, role, content):
    now = int(time.time())
    ttl = now + 14400  # 4 hours

    table.put_item(
        Item={
            "session_id": session_id,
            "timestamp": now,
            "role": role,
            "content": content,
            "expires_at": ttl
        }
    )


def get_history(session_id):
    response = table.query(
        KeyConditionExpression=Key("session_id").eq(session_id),
        ScanIndexForward=True
    )

    items = response.get("Items", [])
    items = items[-8:]  # last 8 messages only

    history_text = ""
    for item in items:
        speaker = "User" if item["role"] == "user" else "Assistant"
        history_text += f"{speaker}: {item['content']}\n"

    return history_text


# ---------- Lambda Handler ----------
def lambda_handler(event, context):
    try:
        body = json.loads(event.get("body", "{}"))

        query = body.get("query")
        session_id = body.get("session_id")

        if not query:
            return {
                "statusCode": 400,
                "body": json.dumps({"error": "Missing query"})
            }

        if not session_id:
            return {
                "statusCode": 400,
                "body": json.dumps({"error": "Missing session_id"})
            }

        # -------- LOAD MEMORY --------
        conversation_history = get_history(session_id)

        # IMPORTANT: prepend memory into the user query
        if conversation_history:
            query = f"""Previous conversation:
{conversation_history}

Current user message:
{query}"""

        # ---------- RAG Call ----------
        rag_response = agent.retrieve_and_generate(
            input={"text": query},
            retrieveAndGenerateConfiguration={
                "type": "KNOWLEDGE_BASE",
                "knowledgeBaseConfiguration": {
                    "knowledgeBaseId": KB_ID,
                    "modelArn": "arn:aws:bedrock:us-east-2:652582397015:inference-profile/global.anthropic.claude-sonnet-4-6",
                    "orchestrationConfiguration": {
                        "promptTemplate": {
                            "textPromptTemplate": ORCHESTRATION_PROMPT
                        }
                    },
                    "generationConfiguration": {
                        "promptTemplate": {
                            "textPromptTemplate": GENERATION_PROMPT
                        }
                    }
                }
            }
        )

        answer = rag_response["output"]["text"]

        # -------- SAVE MEMORY --------
        save_message(session_id, "user", body.get("query"))
        save_message(session_id, "assistant", answer)

        # ---------- Source Retrieval ----------
        retrieval = agent.retrieve(
            knowledgeBaseId=KB_ID,
            retrievalQuery={"text": body.get("query")},
            retrievalConfiguration={
                "vectorSearchConfiguration": {
                    "numberOfResults": 5
                }
            }
        )

        sources = []
        for item in retrieval.get("retrievalResults", []):
            s3 = item.get("location", {}).get("s3Location", {})
            if "uri" in s3:
                sources.append(s3["uri"])

        return {
            "statusCode": 200,
            "body": json.dumps({
                "response": answer,
                "sources": list(set(sources))
            })
        }

    except Exception as e:
        return {
            "statusCode": 500,
            "body": json.dumps({"error": str(e)})
        }
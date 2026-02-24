import json
import boto3
import os

KB_ID = os.environ["KB_ID"]

agent = boto3.client("bedrock-agent-runtime")

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

WHAT YOU SHOUL DO WHILE WAITING FOR EMERGENCY SERVICES: 
A few sentence paragraph explaining what to do while waiting for emergency services to arrive
"""

def lambda_handler(event, context):
    try:
        body = json.loads(event.get("body", "{}"))
        query = body.get("query")

        if not query:
            return {
                "statusCode": 400,
                "body": json.dumps({"error": "Missing query"})
            }

        # Generate response using RAG from knowledge base
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

        # Retrieve sources for debug
        retrieval = agent.retrieve(
            knowledgeBaseId=KB_ID,
            retrievalQuery={"text": query},
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

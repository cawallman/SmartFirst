from smartfirst import ask_smartfirst

print("\n--- CALL 1 ---")
response, sources = ask_smartfirst(
    "My friend Jake is having an allergic reaction. What should I do? Please refer to him as Jake in our conversation.",
    show_sources=True
)

print(response)

print("\nSources Used:")
for s in sources:
    print("-", s)
    
    
    
print("\n--- CALL 2 ---")
response, sources = ask_smartfirst(
    "He says he has an epipen in his bag. How do i use it?",
    show_sources=True
)

print(response)

print("\nSources Used:")
for s in sources:
    print("-", s)
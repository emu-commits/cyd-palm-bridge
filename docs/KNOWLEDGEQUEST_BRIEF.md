<!-- The owner's opportunity brief, as received 2026-11-07. The evaluation and plan are KNOWLEDGEQUEST_PLAN.md. -->

## Opportunity Brief: KnowledgeQuest  
## A handheld operating system for exploring humanity's knowledge as a game  
  
## Executive Summary  
Most educational technology is organized around courses, grades, subjects, and curricula. Human curiosity is not.  
People rarely wake up wanting to study "Chapter 7 of Physics." They become interested in a question, discover a concept, and then want to know what lies beyond it.  
KnowledgeQuest reimagines learning as exploration of a giant prerequisite graph of human knowledge. Instead of books, lessons, or feeds, users traverse a world of interconnected concepts where every learned idea unlocks new paths.  
The experience combines the progression mechanics of a roguelike game, the discoverability of Wikipedia, and the long-term growth of a skill tree.  
Unlike modern learning platforms, it is fully offline, intentionally distraction-free, and designed for short daily interactions on dedicated hardware.  
  
## Problem  
Existing educational products suffer from one of three failure modes:  
## Course-Based Learning  
Platforms such as Coursera, Khan Academy, and university courses require commitment to a predefined path.  
Users must decide:  
* what to learn   
* why to learn it   
* how far to go   
The friction is high.  
  
## Encyclopedia-Based Learning  
Wikipedia offers nearly unlimited knowledge but no structure.  
Users quickly encounter:  
* information overload   
* rabbit holes   
* unclear prerequisites   
* difficulty measuring progress   
The experience is powerful but unbounded.  
  
## Flashcard-Based Learning  
Spaced repetition works.  
The problem is that it answers:  
  
What should I remember?  
  
Not:  
  
What should I discover next?  
  
It optimizes retention but not exploration.  
  
## Core Insight  
Human knowledge can be represented as a graph.  
Every idea has prerequisite ideas.  
For example:  
```
Numbers
  ↓
Fractions
  ↓
Ratios
  ↓
Slope
  ↓
Derivatives
```
Or:  
```
Atoms
  ↓
Electrons
  ↓
Electric Current
  ↓
Circuits
  ↓
Logic Gates
  ↓
Computers
```
Learning becomes navigation.  
  
## Vision  
Imagine opening a pocket-sized device.  
The home screen shows:  
```
TODAY'S DISCOVERIES

✓ Binary Numbers
✓ Probability
✓ Newton's Laws

AVAILABLE PATHS

→ Cryptography
→ Statistics
→ Orbit Mechanics
```
You choose a path.  
The device teaches a single concept.  
Two minutes later:  
```
NODE MASTERED

NEWLY UNLOCKED

→ Public Key Encryption
→ Data Compression
→ Error Correction
```
The session is complete.  
You close the device.  
Tomorrow, more paths await.  
  
## Product Principles  
## One Concept Per Session  
Every interaction should feel achievable.  
No lectures.  
No courses.  
No forty-minute videos.  
Instead:  
* one idea   
* one explanation   
* one diagram   
* one question   
Completion time:  
30 seconds to 3 minutes.  
  
## Progress Through Discovery  
Users should never ask:  
  
What do I learn next?  
  
The graph answers automatically.  
Every completed node reveals adjacent possibilities.  
  
## No Infinite Feed  
The system intentionally avoids:  
* social media   
* notifications   
* engagement farming   
* short-form content   
The goal is anticipation, not addiction.  
  
## Offline First  
Entire experience functions without internet.  
Benefits:  
* focused attention   
* privacy   
* longevity   
* dedicated hardware support   
This is especially important for childhood learning and intentional learning environments.  
  
## Why Dedicated Hardware?  
Surprisingly, the constraint becomes the advantage.  
Phones are optimized for:  
* communication   
* entertainment   
* interruptions   
KnowledgeQuest is optimized for:  
* learning   
* exploration   
* reflection   
A device the size of a Palm Pilot or Game Boy creates a different psychological relationship.  
Opening it means:  
  
I'm here to discover something.  
  
Not:  
  
I'm here to check messages.  
  
  
## The Knowledge Engine  
## Atomic Knowledge Nodes  
Each node consists of:  
```
{
  "id": "derivatives",
  "title": "Derivatives",
  "summary": "...",
  "diagram": "...",
  "question": "...",
  "answer": "...",
  "prerequisites": [...]
}
```
The node is intentionally tiny.  
Learning remains lightweight.  
  
## Global Knowledge Graph  
Potential scale:  
* 20,000-50,000 concepts   
* 100,000-300,000 prerequisite relationships   
Coverage:  
* mathematics   
* physics   
* chemistry   
* biology   
* computer science    
* engineering   
* economics  
* Accounting  
* Finance   
* linguistics   
* history   
* philosophy   
* astronomy    
* psychology   
  
## Procedural Curriculum  
Unlike schools, the curriculum emerges naturally.  
Two learners may begin together and diverge dramatically.  
One becomes interested in:  
```
Math
 → Physics
   → Electronics
     → Computer Architecture
```
Another follows:  
```
Biology
 → Genetics
   → Evolution
     → Ecology
```
Every learner builds a unique path.  
  
## The Game Layer  
The experience borrows from game design rather than educational software.  
## Fog of Knowledge  
Not all future concepts are visible.  
Some branches are hidden.  
Users are motivated by:  
  
What lies behind this path?  
  
  
## Skill Trees  
Completed nodes reveal new opportunities.  
The graph itself becomes content.  
  
## Exploration  
The objective is not completion.  
The objective is curiosity.  
There may never be a final endpoint.  
  
## Personal Knowledge Map  
After months of use, users possess:  
* a visual map of what they know   
* visible gaps   
* unexplored frontiers   
Knowledge becomes tangible.  
Consider ability to sync knowledge externally to use for context (GitHub?)  
  
## Why Now?  
Historically this idea was impossible.  
Creating and maintaining a graph of human knowledge would require enormous manual effort.  
Modern AI changes the equation.  
Large language models can assist in:  
* concept decomposition   
* prerequisite mapping   
* lesson generation   
* question generation   
* consistency checking   
Human experts become graph curators rather than content authors.  
  
## Potential Users  
## Curious Adults  
People who browse Wikipedia for fun.  
## Lifelong Learners  
Continuous personal growth.  
## Builders and Hackers  
Engineers, makers, tinkerers, scientists.  
People who naturally ask:  
How does this work?  
  
  
## Long-Term Vision  
Most software competes for attention.  
KnowledgeQuest competes for curiosity.  
The ultimate ambition is not another educational platform.  
It is a portable map of civilization's knowledge, compressed into a handheld device, where every bite-sized interaction expands the user's understanding of the world.  
The product succeeds when users stop asking:  
  
What should I learn?  
  
and instead start asking:  
  
Where should I explore next?  
  
Reference [https://github.com/withmarbleapp/os-taxonomy](https://github.com/withmarbleapp/os-taxonomy) for content and concepts.   

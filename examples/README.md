# Examples

One Jupyter notebook per use case. Each one builds a fictional problem from Python objects, solves
it with optimaru, draws the result, and then changes the rules to show what happens. The notebooks
are saved with their outputs, so you can read them on GitHub without running anything.

## Run them

```bash
pip install optimaru matplotlib jupyter
jupyter lab
```

Requires Python 3.11+. Each notebook stands alone, so you can download a single file and run it.

Solves stop on a time limit, so your scores and charts can differ a little from the saved outputs,
depending on your machine.

## Where to start

New to optimaru? Start with [`vehicle-routing.ipynb`](vehicle-routing.ipynb). It is the most fully
explained notebook, and the others follow the same outline:

1. Describe the problem as Python objects.
2. Solve it and read the score.
3. Visualize the solution in the form the domain uses: a map, a Gantt chart, a timetable.
4. Check the result without relying on the solver.
5. Change the rules with constraint plugins, without touching the solver.
6. Try a what-if, then restart the search from an existing solution.

## Notebooks

### Routing and logistics

| Notebook | The question it answers |
| --- | --- |
| [Vehicle routing](vehicle-routing.ipynb) | Which truck serves which customer, in what order, within capacity and time windows? |
| [Pickup and delivery](pickup-delivery.ipynb) | How do you route vehicles when every order has a pickup that must come before its dropoff? |
| [Order picking](order-picking.ipynb) | How do trolleys walk a warehouse to collect the items of many orders? |
| [Facility location](facility-location.ipynb) | Which facilities should open, and which consumers should each one serve? |

### Scheduling

| Notebook | The question it answers |
| --- | --- |
| [Employee scheduling](employee-scheduling.ipynb) | Who works which shift, given coverage, skills and rest rules? |
| [Flight crew scheduling](flight-crew-scheduling.ipynb) | Which crew member takes each seat on each flight? |
| [Maintenance scheduling](maintenance-scheduling.ipynb) | When does each job run, and which crew does it? |
| [Project job scheduling](project-job-scheduling.ipynb) | How do you sequence the jobs of several projects on shared resources? |
| [Semiconductor scheduling](semiconductor-scheduling.ipynb) | Which machine runs each lot operation, and when? |
| [Food packaging](food-packaging.ipynb) | In what order should each production line run its jobs, given cleaning time between products? |
| [Task assigning](task-assigning.ipynb) | Which employee takes each task, and in what order? |
| [Meeting scheduling](meeting-scheduling.ipynb) | When and where does each meeting happen, so that the people who need to attend can? |

### Timetabling

| Notebook | The question it answers |
| --- | --- |
| [School timetabling](school-timetabling.ipynb) | When does each class meet each teacher during the week? |
| [Conference scheduling](conference-scheduling.ipynb) | Which timeslot and room does each talk get? |
| [Sports league scheduling](sports-league-scheduling.ipynb) | Who plays whom in each round, and at home or away? |
| [Tournament scheduling](tournament-scheduling.ipynb) | Which teams play on which court each day, fairly? |

### Allocation and packing

| Notebook | The question it answers |
| --- | --- |
| [Bed allocation](bed-allocation.ipynb) | Which bed does each patient get for their stay? |
| [Bin packing](bin-packing.ipynb) | What is the smallest number of bins that holds every item? |
| [Knapsack](knapsack.ipynb) | Which items give the most value within a weight limit? |
| [Assembly-line balancing](assembly-line-balancing.ipynb) | How do you split ordered tasks across as few stations as possible? |
| [Quadratic assignment](quadratic-assignment.ipynb) | Where should each facility go, so that heavy flows travel short distances? |
| [Max-cut](max-cut.ipynb) | How do you split a graph in two to maximize the weight of the edges between the sides? |

## Going further

- [`use-cases/README.md`](../use-cases/README.md) explains how to add a move, change the construction
  heuristic, or add a hard or soft cost.
- Each use case's README covers its usage and how to extend it.

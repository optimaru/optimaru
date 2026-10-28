package optimaru.bench.timefold;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Collection;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ai.timefold.solver.core.api.domain.entity.PlanningEntity;
import ai.timefold.solver.core.api.domain.common.PlanningId;
import ai.timefold.solver.core.api.domain.solution.PlanningEntityCollectionProperty;
import ai.timefold.solver.core.api.domain.variable.InverseRelationShadowVariable;
import ai.timefold.solver.core.api.domain.variable.PlanningListVariable;
import ai.timefold.solver.core.api.domain.variable.PlanningVariable;

import com.fasterxml.jackson.annotation.JsonIdentityInfo;

/**
 * Reflection helpers that keep the JSON contract with the Python adapters
 * independent of each quickstart's Jackson details.
 *
 * <p>{@link #relink} makes every reference to an identified object (one with
 * a {@code @PlanningId}, or a {@code @JsonIdentityInfo} property) point at the
 * one instance held by the solution's top-level lists, so an adapter may write
 * a reference as {@code {"id": "..."}} or as a full copy. {@link #assignment}
 * reads every planning variable back as ids.
 */
final class Graph {

    private Graph() {
    }

    static void relink(Object solution) throws IllegalAccessException {
        Map<Class<?>, Map<Object, Object>> index = new LinkedHashMap<>();
        for (Field field : fields(solution.getClass())) {
            Object value = field.get(solution);
            if (value instanceof Collection<?> collection) {
                for (Object element : collection) {
                    register(index, element);
                }
            } else if (value != null) {
                register(index, value);
            }
        }
        Set<Object> visited = java.util.Collections.newSetFromMap(new IdentityHashMap<>());
        walk(solution, index, visited);
        // Inverse-relation collections the JSON leaves out must exist (empty)
        // for Timefold to fill them.
        for (Map<Object, Object> byId : index.values()) {
            for (Object object : byId.values()) {
                for (Field field : fields(object.getClass())) {
                    if (field.isAnnotationPresent(InverseRelationShadowVariable.class)
                            && Collection.class.isAssignableFrom(field.getType()) && field.get(object) == null) {
                        field.set(object, new ArrayList<>());
                    }
                }
            }
        }
    }

    private static void register(Map<Class<?>, Map<Object, Object>> index, Object object) throws IllegalAccessException {
        Object id = idOf(object);
        if (id != null) {
            index.computeIfAbsent(object.getClass(), c -> new LinkedHashMap<>()).putIfAbsent(id, object);
        }
    }

    private static Object canonical(Map<Class<?>, Map<Object, Object>> index, Object object)
            throws IllegalAccessException {
        if (object == null) {
            return null;
        }
        Map<Object, Object> byId = index.get(object.getClass());
        if (byId == null) {
            return object;
        }
        Object id = idOf(object);
        return id == null ? object : byId.getOrDefault(id, object);
    }

    @SuppressWarnings({ "unchecked", "rawtypes" })
    private static void walk(Object object, Map<Class<?>, Map<Object, Object>> index, Set<Object> visited)
            throws IllegalAccessException {
        if (object == null || !visited.add(object) || isValue(object.getClass())) {
            return;
        }
        for (Field field : fields(object.getClass())) {
            Object value = field.get(object);
            if (value instanceof List list) {
                for (int i = 0; i < list.size(); i++) {
                    Object linked = canonical(index, list.get(i));
                    if (linked != list.get(i)) {
                        list.set(i, linked);
                    }
                    walk(linked, index, visited);
                }
            } else if (value instanceof Collection collection) {
                List<Object> linked = new ArrayList<>();
                for (Object element : collection) {
                    linked.add(canonical(index, element));
                }
                collection.clear();
                collection.addAll(linked);
                for (Object element : linked) {
                    walk(element, index, visited);
                }
            } else if (value instanceof Map map) {
                Map<Object, Object> linked = new LinkedHashMap<>();
                for (Object entry : map.entrySet()) {
                    Map.Entry e = (Map.Entry) entry;
                    linked.put(canonical(index, e.getKey()), canonical(index, e.getValue()));
                }
                map.clear();
                map.putAll(linked);
                for (Object key : linked.keySet()) {
                    walk(key, index, visited);
                }
                for (Object item : linked.values()) {
                    walk(item, index, visited);
                }
            } else if (value != null && !field.getType().isPrimitive()) {
                Object linked = canonical(index, value);
                if (linked != value && !Modifier.isFinal(field.getModifiers())) {
                    field.set(object, linked);
                }
                walk(linked, index, visited);
            }
        }
    }

    /**
     * Sets list variables a quickstart does not (de)serialize, from
     * {@code {"EntityClass": {"entityId": {"variable": [element ids]}}}}.
     */
    static void applyLists(Object solution, Map<String, Map<String, Map<String, List<String>>>> lists)
            throws IllegalAccessException {
        Map<String, Object> elementsById = new LinkedHashMap<>();
        Map<String, Map<String, Object>> entitiesByClass = new LinkedHashMap<>();
        for (Field field : fields(solution.getClass())) {
            if (field.get(solution) instanceof Collection<?> collection) {
                for (Object element : collection) {
                    Object id = idOf(element);
                    if (id != null) {
                        elementsById.putIfAbsent(element.getClass().getSimpleName() + ":" + id, element);
                        entitiesByClass.computeIfAbsent(element.getClass().getSimpleName(), k -> new LinkedHashMap<>())
                                .put(String.valueOf(id), element);
                    }
                }
            }
        }
        for (var byEntity : lists.entrySet()) {
            for (var byId : byEntity.getValue().entrySet()) {
                Object entity = entitiesByClass.get(byEntity.getKey()).get(byId.getKey());
                for (var variable : byId.getValue().entrySet()) {
                    Field field = fields(entity.getClass()).stream()
                            .filter(f -> f.getName().equals(variable.getKey())).findFirst().orElseThrow();
                    if (!field.isAnnotationPresent(PlanningListVariable.class)) {
                        throw new IllegalArgumentException(variable.getKey() + " is not a list variable");
                    }
                    Class<?> elementType = (Class<?>) ((java.lang.reflect.ParameterizedType) field.getGenericType())
                            .getActualTypeArguments()[0];
                    List<Object> elements = new ArrayList<>();
                    for (String id : variable.getValue()) {
                        Object element = elementsById.get(elementType.getSimpleName() + ":" + id);
                        if (element == null) {
                            throw new IllegalArgumentException("unknown " + elementType.getSimpleName() + " " + id);
                        }
                        elements.add(element);
                    }
                    field.set(entity, elements);
                }
            }
        }
    }

    /** {"EntityClass": {"entityId": {"variable": id | [ids] | value}}}. */
    static Map<String, Map<String, Map<String, Object>>> assignment(Object solution) throws IllegalAccessException {
        Map<String, Map<String, Map<String, Object>>> out = new LinkedHashMap<>();
        for (Field field : fields(solution.getClass())) {
            if (!field.isAnnotationPresent(PlanningEntityCollectionProperty.class)) {
                continue;
            }
            for (Object entity : (Collection<?>) field.get(solution)) {
                Map<String, Object> variables = new LinkedHashMap<>();
                for (Field variable : fields(entity.getClass())) {
                    Object value = variable.get(entity);
                    if (variable.isAnnotationPresent(PlanningVariable.class)) {
                        variables.put(variable.getName(), value == null ? null : idOrValue(value));
                    } else if (variable.isAnnotationPresent(PlanningListVariable.class)) {
                        List<Object> ids = new ArrayList<>();
                        for (Object element : (Collection<?>) value) {
                            ids.add(idOrValue(element));
                        }
                        variables.put(variable.getName(), ids);
                    }
                }
                if (!variables.isEmpty()) {
                    out.computeIfAbsent(entity.getClass().getSimpleName(), k -> new LinkedHashMap<>())
                            .put(String.valueOf(idOf(entity)), variables);
                }
            }
        }
        return out;
    }

    private static Object idOrValue(Object value) throws IllegalAccessException {
        Object id = idOf(value);
        return id != null ? String.valueOf(id) : value.toString();
    }

    static Object idOf(Object object) throws IllegalAccessException {
        if (object == null || isValue(object.getClass())) {
            return null;
        }
        String identityProperty = null;
        JsonIdentityInfo info = object.getClass().getAnnotation(JsonIdentityInfo.class);
        if (info != null) {
            identityProperty = info.property();
        }
        for (Field field : fields(object.getClass())) {
            if (field.isAnnotationPresent(PlanningId.class) || field.getName().equals(identityProperty)) {
                return field.get(object);
            }
        }
        return null;
    }

    private static boolean isValue(Class<?> type) {
        return type.isPrimitive() || type.isEnum() || type.getName().startsWith("java.")
                || type.getName().startsWith("ai.timefold.");
    }

    private static List<Field> fields(Class<?> type) {
        List<Field> result = new ArrayList<>();
        for (Class<?> c = type; c != null && c != Object.class; c = c.getSuperclass()) {
            for (Field field : c.getDeclaredFields()) {
                if (!Modifier.isStatic(field.getModifiers())) {
                    field.setAccessible(true);
                    result.add(field);
                }
            }
        }
        return result;
    }

    static boolean isEntity(Class<?> type) {
        return type.isAnnotationPresent(PlanningEntity.class);
    }
}
